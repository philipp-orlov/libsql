// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "reports.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace sqlweb {

namespace {

// A strict JSON checker: no extensions, no trailing commas, no comments, escapes and numbers as RFC 8259 has them.
class JsonCheck
{
public:
    explicit JsonCheck(std::string_view text) : s_(text) {}

    bool run()
    {
        skip();
        if (!value(0))
            return false;
        skip();
        return i_ == s_.size();
    }

private:
    void skip()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }
    bool literal(std::string_view word)
    {
        if (s_.substr(i_, word.size()) != word)
            return false;
        i_ += word.size();
        return true;
    }
    bool value(int depth)
    {
        if (depth > 64 || i_ >= s_.size())
            return false;
        switch (s_[i_]) {
        case '{': return object(depth);
        case '[': return array(depth);
        case '"': return string();
        case 't': return literal("true");
        case 'f': return literal("false");
        case 'n': return literal("null");
        default: return number();
        }
    }
    bool object(int depth)
    {
        ++i_;
        skip();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        for (;;) {
            skip();
            if (i_ >= s_.size() || s_[i_] != '"' || !string())
                return false;
            skip();
            if (i_ >= s_.size() || s_[i_++] != ':')
                return false;
            skip();
            if (!value(depth + 1))
                return false;
            skip();
            if (i_ >= s_.size())
                return false;
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            return s_[i_++] == '}';
        }
    }
    bool array(int depth)
    {
        ++i_;
        skip();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        for (;;) {
            skip();
            if (!value(depth + 1))
                return false;
            skip();
            if (i_ >= s_.size())
                return false;
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            return s_[i_++] == ']';
        }
    }
    bool string()
    {
        ++i_;
        while (i_ < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[i_++]);
            if (c == '"')
                return true;
            if (c < 0x20)
                return false;
            if (c == '\\') {
                if (i_ >= s_.size())
                    return false;
                const char e = s_[i_++];
                if (e == 'u') {
                    for (int k = 0; k < 4; ++k)
                        if (i_ >= s_.size() || !std::isxdigit(static_cast<unsigned char>(s_[i_++])))
                            return false;
                } else if (std::string_view("\"\\/bfnrt").find(e) == std::string_view::npos) {
                    return false;
                }
            }
        }
        return false;
    }
    bool digits()
    {
        const std::size_t start = i_;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9')
            ++i_;
        return i_ > start;
    }
    bool number()
    {
        if (i_ < s_.size() && s_[i_] == '-')
            ++i_;
        if (i_ < s_.size() && s_[i_] == '0')
            ++i_;
        else if (!digits())
            return false;
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (!digits())
                return false;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-'))
                ++i_;
            if (!digits())
                return false;
        }
        return true;
    }

    std::string_view s_;
    std::size_t i_ = 0;
};

}  // namespace

bool ReportStore::validName(std::string_view name)
{
    if (name.empty() || name.size() > 80 || name.back() == '.')
        return false;
    if (!std::isalnum(static_cast<unsigned char>(name.front())))
        return false;
    for (char c : name) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-' || c == '.';
        if (!ok || static_cast<unsigned char>(c) >= 0x80)
            return false;
    }
    return name.find("..") == std::string_view::npos;
}

bool ReportStore::validJson(std::string_view text)
{
    return JsonCheck(text).run();
}

std::filesystem::path ReportStore::file(std::string_view name) const
{
    return directory_ / (std::string(name) + ".json");
}

std::vector<ReportInfo> ReportStore::list() const
{
    std::vector<ReportInfo> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory_, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& path = it->path();
        if (path.extension() != ".json" || !it->is_regular_file(ec))
            continue;
        const std::string name = path.stem().string();
        if (!validName(name))
            continue;
        ReportInfo info;
        info.name = name;
        info.size = static_cast<std::uint64_t>(it->file_size(ec));
        // file_clock has no portable conversion before C++20: measure against "now" on both clocks
        const auto age = std::filesystem::file_time_type::clock::now() - it->last_write_time(ec);
        info.modified = std::chrono::duration_cast<std::chrono::seconds>(
                            (std::chrono::system_clock::now() - age).time_since_epoch())
                            .count();
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const ReportInfo& a, const ReportInfo& b) { return a.name < b.name; });
    return out;
}

std::optional<std::string> ReportStore::read(std::string_view name) const
{
    if (!validName(name))
        return std::nullopt;
    std::ifstream in(file(name), std::ios::binary);
    if (!in)
        return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

void ReportStore::write(std::string_view name, std::string_view json) const
{
    if (!validName(name))
        throw std::runtime_error("invalid report name");
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec)
        throw std::runtime_error("cannot create " + directory_.string() + ": " + ec.message());
    const auto target = file(name);
    auto temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        out.flush();
        if (!out)
            throw std::runtime_error("cannot write " + temporary.string());
    }
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        throw std::runtime_error("cannot replace " + target.string());
    }
}

bool ReportStore::remove(std::string_view name) const
{
    if (!validName(name))
        return false;
    std::error_code ec;
    return std::filesystem::remove(file(name), ec);
}

}  // namespace sqlweb
