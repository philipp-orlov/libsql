// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "internal/func.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sql::internal {
namespace {

enum class JKind : std::uint8_t
{
    Null,
    True,
    False,
    Integer,
    Real,
    Text,
    Array,
    Object,
};

struct Member;

struct Node
{
    JKind kind = JKind::Null;
    std::string text;
    std::string raw;
    std::uint8_t jsonbType = 0;
    std::vector<Node> array;
    std::vector<Member> object;
    std::size_t id = 0;
};

struct Member
{
    Node key;
    Node value;
    std::size_t id = 0;
};

bool isSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool isHex(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

unsigned hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return unsigned(c - '0');
    if (c >= 'a' && c <= 'f')
        return unsigned(c - 'a' + 10);
    return unsigned(c - 'A' + 10);
}

bool isIdStart(unsigned char c) noexcept
{
    return c == '_' || c == '$' || std::isalpha(c) || c >= 0x80;
}

bool isIdContinue(unsigned char c) noexcept
{
    return isIdStart(c) || std::isdigit(c);
}

[[noreturn]] void malformed(FuncContext& ctx)
{
    ctx.error("malformed JSON");
}

[[noreturn]] void tooDeep(FuncContext& ctx)
{
    ctx.error("malformed JSON");
}

void appendJsonString(std::string_view text, std::string& out)
{
    out.push_back('"');
    for (const unsigned char ch : text) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[ch >> 4]);
                    out.push_back(kHex[ch & 0xf]);
                } else {
                    out.push_back(char(ch));
                }
                break;
        }
    }
    out.push_back('"');
}

void appendCodepoint(std::uint32_t cp, std::string& out)
{
    if (cp <= 0x7f) {
        out.push_back(char(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(char(0xc0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(char(0xe0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(char(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(char(0xf0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(char(0x80 | (cp & 0x3f)));
    }
}

class JsonParser
{
public:
    JsonParser(FuncContext& context, std::string_view input, bool json5)
        : ctx_(context), in_(input), json5_(json5)
    {}

    Node parse()
    {
        skipSpace();
        Node out = parseValue(0);
        skipSpace();
        if (pos_ != in_.size())
            malformed(ctx_);
        return out;
    }

    std::size_t errorOffset()
    {
        try {
            (void)parse();
            return 0;
        } catch (const Error&) {
            return pos_ + 1;
        }
    }

private:
    void skipSpace()
    {
        for (;;) {
            while (pos_ < in_.size() && isSpace(in_[pos_]))
                ++pos_;
            if (!json5_ || pos_ + 1 >= in_.size() || in_[pos_] != '/')
                return;
            if (in_[pos_ + 1] == '/') {
                pos_ += 2;
                while (pos_ < in_.size() && in_[pos_] != '\n' && in_[pos_] != '\r')
                    ++pos_;
            } else if (in_[pos_ + 1] == '*') {
                pos_ += 2;
                while (pos_ + 1 < in_.size() && !(in_[pos_] == '*' && in_[pos_ + 1] == '/'))
                    ++pos_;
                if (pos_ + 1 >= in_.size())
                    malformed(ctx_);
                pos_ += 2;
            } else {
                return;
            }
        }
    }

    Node parseValue(unsigned depth)
    {
        if (depth > 1000)
            tooDeep(ctx_);
        skipSpace();
        if (pos_ >= in_.size())
            malformed(ctx_);
        const char c = in_[pos_];
        if (c == '{')
            return parseObject(depth + 1);
        if (c == '[')
            return parseArray(depth + 1);
        if (c == '"' || (json5_ && c == '\''))
            return parseString();
        if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9'))
            return parseNumber();
        if (match("null")) {
            Node n;
            n.kind = JKind::Null;
            n.jsonbType = 0;
            return n;
        }
        if (match("true")) {
            Node n;
            n.kind = JKind::True;
            n.jsonbType = 1;
            return n;
        }
        if (match("false")) {
            Node n;
            n.kind = JKind::False;
            n.jsonbType = 2;
            return n;
        }
        if (json5_ && (matchNoCase("Infinity") || matchNoCase("Inf"))) {
            Node n;
            n.kind = JKind::Real;
            n.raw = "9e999";
            n.text = "9e999";
            n.jsonbType = 5;
            return n;
        }
        if (json5_ && matchNoCase("NaN")) {
            Node n;
            n.kind = JKind::Null;
            n.jsonbType = 0;
            return n;
        }
        malformed(ctx_);
    }

    bool match(std::string_view token)
    {
        if (in_.substr(pos_, token.size()) != token)
            return false;
        pos_ += token.size();
        return true;
    }

    bool matchNoCase(std::string_view token)
    {
        if (pos_ + token.size() > in_.size())
            return false;
        for (std::size_t i = 0; i < token.size(); ++i) {
            if (foldAscii(in_[pos_ + i]) != foldAscii(token[i]))
                return false;
        }
        pos_ += token.size();
        return true;
    }

    Node parseArray(unsigned depth)
    {
        Node out;
        out.kind = JKind::Array;
        out.jsonbType = 11;
        ++pos_;
        skipSpace();
        if (pos_ < in_.size() && in_[pos_] == ']') {
            ++pos_;
            return out;
        }
        for (;;) {
            out.array.push_back(parseValue(depth));
            skipSpace();
            if (pos_ >= in_.size())
                malformed(ctx_);
            if (in_[pos_] == ']') {
                ++pos_;
                return out;
            }
            if (in_[pos_] != ',')
                malformed(ctx_);
            ++pos_;
            skipSpace();
            if (json5_ && pos_ < in_.size() && in_[pos_] == ']') {
                ++pos_;
                return out;
            }
        }
    }

    Node parseObject(unsigned depth)
    {
        Node out;
        out.kind = JKind::Object;
        out.jsonbType = 12;
        ++pos_;
        skipSpace();
        if (pos_ < in_.size() && in_[pos_] == '}') {
            ++pos_;
            return out;
        }
        for (;;) {
            Node key;
            if (pos_ < in_.size() && (in_[pos_] == '"' || (json5_ && in_[pos_] == '\''))) {
                key = parseString();
            } else if (json5_ && pos_ < in_.size() && isIdStart(static_cast<unsigned char>(in_[pos_]))) {
                const std::size_t start = pos_;
                while (pos_ < in_.size() && isIdContinue(static_cast<unsigned char>(in_[pos_])))
                    ++pos_;
                key.kind = JKind::Text;
                key.text.assign(in_.substr(start, pos_ - start));
                key.raw = key.text;
                key.jsonbType = 7;
            } else {
                malformed(ctx_);
            }
            skipSpace();
            if (pos_ >= in_.size() || in_[pos_] != ':')
                malformed(ctx_);
            ++pos_;
            Member m;
            m.key = std::move(key);
            m.value = parseValue(depth);
            out.object.push_back(std::move(m));
            skipSpace();
            if (pos_ >= in_.size())
                malformed(ctx_);
            if (in_[pos_] == '}') {
                ++pos_;
                return out;
            }
            if (in_[pos_] != ',')
                malformed(ctx_);
            ++pos_;
            skipSpace();
            if (json5_ && pos_ < in_.size() && in_[pos_] == '}') {
                ++pos_;
                return out;
            }
        }
    }

    std::uint32_t parseHex4()
    {
        if (pos_ + 4 > in_.size())
            malformed(ctx_);
        std::uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            if (!isHex(in_[pos_ + std::size_t(i)]))
                malformed(ctx_);
            cp = (cp << 4) | hexValue(in_[pos_ + std::size_t(i)]);
        }
        pos_ += 4;
        return cp;
    }

    Node parseString()
    {
        const char quote = in_[pos_++];
        Node out;
        out.kind = JKind::Text;
        out.jsonbType = 7;
        std::string raw;
        bool hadEscape = false;
        bool hadJson5Escape = false;
        bool needsEscape = false;
        while (pos_ < in_.size()) {
            const unsigned char c = static_cast<unsigned char>(in_[pos_++]);
            if (c == static_cast<unsigned char>(quote)) {
                out.raw = hadEscape ? raw : out.text;
                out.jsonbType = hadJson5Escape ? 9 : (hadEscape ? 8 : (needsEscape ? 10 : 7));
                return out;
            }
            if (c == '\\') {
                hadEscape = true;
                if (pos_ >= in_.size())
                    malformed(ctx_);
                const char esc = in_[pos_++];
                raw.push_back('\\');
                raw.push_back(esc);
                switch (esc) {
                    case '"':
                    case '\\':
                    case '/':
                        out.text.push_back(esc);
                        break;
                    case 'b':
                        out.text.push_back('\b');
                        break;
                    case 'f':
                        out.text.push_back('\f');
                        break;
                    case 'n':
                        out.text.push_back('\n');
                        break;
                    case 'r':
                        out.text.push_back('\r');
                        break;
                    case 't':
                        out.text.push_back('\t');
                        break;
                    case 'u': {
                        const std::size_t hexStart = pos_;
                        std::uint32_t cp = parseHex4();
                        raw.append(in_.substr(hexStart, 4));
                        if (cp >= 0xd800 && cp <= 0xdbff && pos_ + 6 <= in_.size() &&
                            in_[pos_] == '\\' && in_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            const std::size_t lowStart = pos_;
                            const std::uint32_t low = parseHex4();
                            raw += "\\u";
                            raw.append(in_.substr(lowStart, 4));
                            if (low >= 0xdc00 && low <= 0xdfff)
                                cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                        }
                        appendCodepoint(cp, out.text);
                        break;
                    }
                    case '\'':
                    case 'v':
                    case '0':
                        if (!json5_)
                            malformed(ctx_);
                        hadJson5Escape = true;
                        out.text.push_back(esc == 'v' ? '\v' : (esc == '0' ? '\0' : '\''));
                        break;
                    case '\n':
                    case '\r':
                        if (!json5_)
                            malformed(ctx_);
                        hadJson5Escape = true;
                        break;
                    default:
                        if (!json5_)
                            malformed(ctx_);
                        hadJson5Escape = true;
                        out.text.push_back(esc);
                        break;
                }
            } else {
                if (c < 0x20)
                    malformed(ctx_);
                out.text.push_back(char(c));
                raw.push_back(char(c));
                if (c == '"' || c == '\\' || c < 0x20)
                    needsEscape = true;
            }
        }
        malformed(ctx_);
    }

    Node parseNumber()
    {
        const std::size_t start = pos_;
        bool negative = false;
        if (in_[pos_] == '+' || in_[pos_] == '-') {
            negative = in_[pos_] == '-';
            ++pos_;
        }
        if (json5_ && (matchNoCase("Infinity") || matchNoCase("Inf"))) {
            Node n;
            n.kind = JKind::Real;
            n.raw = negative ? "-9e999" : "9e999";
            n.text = n.raw;
            n.jsonbType = 5;
            return n;
        }
        if (json5_ && matchNoCase("NaN")) {
            Node n;
            n.kind = JKind::Null;
            n.jsonbType = 0;
            return n;
        }
        if (json5_ && pos_ + 1 < in_.size() && in_[pos_] == '0' &&
            (in_[pos_ + 1] == 'x' || in_[pos_ + 1] == 'X')) {
            pos_ += 2;
            const std::size_t digits = pos_;
            while (pos_ < in_.size() && isHex(in_[pos_]))
                ++pos_;
            if (digits == pos_)
                malformed(ctx_);
            Node n;
            n.kind = JKind::Integer;
            n.raw.assign(in_.substr(start, pos_ - start));
            unsigned long long value = 0;
            for (std::size_t i = digits; i < pos_; ++i)
                value = value * 16 + hexValue(in_[i]);
            n.text = (negative ? "-" : "") + std::to_string(value);
            n.jsonbType = 4;
            return n;
        }

        const std::size_t intStart = pos_;
        while (pos_ < in_.size() && std::isdigit(static_cast<unsigned char>(in_[pos_])))
            ++pos_;
        bool real = false;
        if (pos_ < in_.size() && in_[pos_] == '.') {
            real = true;
            ++pos_;
            while (pos_ < in_.size() && std::isdigit(static_cast<unsigned char>(in_[pos_])))
                ++pos_;
        }
        if (intStart == pos_ && !real)
            malformed(ctx_);
        if (pos_ < in_.size() && (in_[pos_] == 'e' || in_[pos_] == 'E')) {
            real = true;
            ++pos_;
            if (pos_ < in_.size() && (in_[pos_] == '+' || in_[pos_] == '-'))
                ++pos_;
            const std::size_t expStart = pos_;
            while (pos_ < in_.size() && std::isdigit(static_cast<unsigned char>(in_[pos_])))
                ++pos_;
            if (expStart == pos_)
                malformed(ctx_);
        }
        Node n;
        n.kind = real ? JKind::Real : JKind::Integer;
        n.raw.assign(in_.substr(start, pos_ - start));
        n.text = n.raw;
        n.jsonbType = real ? 5 : 3;
        return n;
    }

    FuncContext& ctx_;
    std::string_view in_;
    std::size_t pos_ = 0;
    bool json5_ = true;
};

void appendJson(const Node& n, std::string& out)
{
    switch (n.kind) {
        case JKind::Null:
            out += "null";
            break;
        case JKind::True:
            out += "true";
            break;
        case JKind::False:
            out += "false";
            break;
        case JKind::Integer:
        case JKind::Real:
            out += n.text;
            break;
        case JKind::Text:
            if ((n.jsonbType == 8 || n.jsonbType == 9) && !n.raw.empty()) {
                out.push_back('"');
                out += n.raw;
                out.push_back('"');
            } else {
                appendJsonString(n.text, out);
            }
            break;
        case JKind::Array:
            out.push_back('[');
            for (std::size_t i = 0; i < n.array.size(); ++i) {
                if (i != 0)
                    out.push_back(',');
                appendJson(n.array[i], out);
            }
            out.push_back(']');
            break;
        case JKind::Object:
            out.push_back('{');
            for (std::size_t i = 0; i < n.object.size(); ++i) {
                if (i != 0)
                    out.push_back(',');
                appendJsonString(n.object[i].key.text, out);
                out.push_back(':');
                appendJson(n.object[i].value, out);
            }
            out.push_back('}');
            break;
    }
}

std::string toJson(const Node& n)
{
    std::string out;
    appendJson(n, out);
    return out;
}

void jsonResult(Value& out, const std::string& text)
{
    out.setText(text);
    out.setSubtype(kJsonSubtype);
}

void appendHeader(std::string& out, std::uint8_t type, std::size_t payload)
{
    if (payload <= 11) {
        out.push_back(char((payload << 4) | type));
    } else if (payload <= 0xff) {
        out.push_back(char(0xc0 | type));
        out.push_back(char(payload));
    } else if (payload <= 0xffff) {
        out.push_back(char(0xd0 | type));
        out.push_back(char((payload >> 8) & 0xff));
        out.push_back(char(payload & 0xff));
    } else {
        out.push_back(char(0xe0 | type));
        out.push_back(char((payload >> 24) & 0xff));
        out.push_back(char((payload >> 16) & 0xff));
        out.push_back(char((payload >> 8) & 0xff));
        out.push_back(char(payload & 0xff));
    }
}

void encodeJsonb(const Node& n, std::string& out, std::size_t* offset = nullptr)
{
    if (offset != nullptr)
        const_cast<Node&>(n).id = out.size();
    switch (n.kind) {
        case JKind::Null:
            appendHeader(out, 0, 0);
            break;
        case JKind::True:
            appendHeader(out, 1, 0);
            break;
        case JKind::False:
            appendHeader(out, 2, 0);
            break;
        case JKind::Integer:
        case JKind::Real: {
            const std::string& payload = n.raw.empty() ? n.text : n.raw;
            appendHeader(out, n.jsonbType != 0 ? n.jsonbType : (n.kind == JKind::Integer ? 3 : 5),
                         payload.size());
            out += payload;
            break;
        }
        case JKind::Text: {
            const std::string payload = n.raw.empty() ? n.text : n.raw;
            appendHeader(out, n.jsonbType != 0 ? n.jsonbType : 7, payload.size());
            out += payload;
            break;
        }
        case JKind::Array: {
            std::string payload;
            for (const Node& child : n.array)
                encodeJsonb(child, payload, offset);
            appendHeader(out, 11, payload.size());
            out += payload;
            break;
        }
        case JKind::Object: {
            std::string payload;
            for (const Member& m : n.object) {
                const std::size_t keyOffset = payload.size();
                encodeJsonb(m.key, payload, offset);
                const_cast<Member&>(m).id = keyOffset;
                encodeJsonb(m.value, payload, offset);
            }
            appendHeader(out, 12, payload.size());
            out += payload;
            break;
        }
    }
}

std::string toJsonb(const Node& n)
{
    std::string out;
    encodeJsonb(n, out);
    return out;
}

bool readHeader(std::string_view bytes, std::size_t pos, std::uint8_t& type, std::size_t& payload,
                std::size_t& header)
{
    if (pos >= bytes.size())
        return false;
    const unsigned char first = static_cast<unsigned char>(bytes[pos]);
    type = first & 0x0f;
    const unsigned sizeCode = first >> 4;
    if (sizeCode <= 11) {
        payload = sizeCode;
        header = 1;
        return pos + header + payload <= bytes.size();
    }
    const std::size_t extra = sizeCode == 12 ? 1 : (sizeCode == 13 ? 2 : (sizeCode == 14 ? 4 : 8));
    if (pos + 1 + extra > bytes.size())
        return false;
    payload = 0;
    for (std::size_t i = 0; i < extra; ++i)
        payload = (payload << 8) | static_cast<unsigned char>(bytes[pos + 1 + i]);
    header = 1 + extra;
    return pos + header + payload <= bytes.size();
}

class JsonbParser
{
public:
    JsonbParser(FuncContext& context, std::string_view input) : ctx_(context), in_(input) {}

    Node parse()
    {
        std::size_t pos = 0;
        Node n = parseAt(pos, 0);
        if (pos != in_.size())
            malformed(ctx_);
        return n;
    }

    bool valid()
    {
        try {
            (void)parse();
            return true;
        } catch (const Error&) {
            return false;
        }
    }

private:
    Node parseAt(std::size_t& pos, unsigned depth)
    {
        if (depth > 1000)
            tooDeep(ctx_);
        std::uint8_t type = 0;
        std::size_t payload = 0;
        std::size_t header = 0;
        const std::size_t start = pos;
        if (!readHeader(in_, pos, type, payload, header) || type > 12)
            malformed(ctx_);
        pos += header;
        const std::string_view body = in_.substr(pos, payload);
        pos += payload;
        Node n;
        n.id = start;
        n.jsonbType = type;
        switch (type) {
            case 0:
                n.kind = JKind::Null;
                return n;
            case 1:
                n.kind = JKind::True;
                return n;
            case 2:
                n.kind = JKind::False;
                return n;
            case 3:
            case 4:
                n.kind = JKind::Integer;
                n.raw.assign(body);
                n.text = numberTextFromRaw(n.raw, type);
                return n;
            case 5:
            case 6:
                n.kind = JKind::Real;
                n.raw.assign(body);
                n.text = numberTextFromRaw(n.raw, type);
                return n;
            case 7:
            case 8:
            case 9:
            case 10:
                n.kind = JKind::Text;
                n.raw.assign(body);
                n.text = decodeJsonbString(n.raw, type);
                return n;
            case 11: {
                n.kind = JKind::Array;
                std::size_t child = start + header;
                const std::size_t end = child + payload;
                while (child < end)
                    n.array.push_back(parseAt(child, depth + 1));
                if (child != end)
                    malformed(ctx_);
                return n;
            }
            case 12: {
                n.kind = JKind::Object;
                std::size_t child = start + header;
                const std::size_t end = child + payload;
                while (child < end) {
                    Member m;
                    const std::size_t keyId = child;
                    m.key = parseAt(child, depth + 1);
                    m.id = keyId;
                    if (m.key.kind != JKind::Text)
                        malformed(ctx_);
                    m.value = parseAt(child, depth + 1);
                    n.object.push_back(std::move(m));
                }
                if (child != end)
                    malformed(ctx_);
                return n;
            }
        }
        malformed(ctx_);
    }

    std::string numberTextFromRaw(const std::string& raw, std::uint8_t type)
    {
        if (type == 4 && raw.size() > 2) {
            bool neg = false;
            std::size_t pos = 0;
            if (raw[pos] == '-' || raw[pos] == '+') {
                neg = raw[pos] == '-';
                ++pos;
            }
            if (pos + 1 < raw.size() && raw[pos] == '0' && (raw[pos + 1] == 'x' || raw[pos + 1] == 'X')) {
                pos += 2;
                unsigned long long value = 0;
                while (pos < raw.size() && isHex(raw[pos])) {
                    value = value * 16 + hexValue(raw[pos]);
                    ++pos;
                }
                return (neg ? "-" : "") + std::to_string(value);
            }
        }
        return raw;
    }

    std::string decodeJsonbString(const std::string& raw, std::uint8_t type)
    {
        if (type == 7 || type == 10)
            return raw;
        JsonParser p(ctx_, "\"" + raw + "\"", true);
        return p.parse().text;
    }

    FuncContext& ctx_;
    std::string_view in_;
};

bool isValidJsonb(std::string_view bytes)
{
    std::size_t payload = 0;
    std::size_t header = 0;
    std::uint8_t type = 0;
    return readHeader(bytes, 0, type, payload, header) && type <= 12 && header + payload == bytes.size();
}

std::string textValue(const Value& value, std::string& scratch)
{
    scratch.clear();
    appendText(value, scratch);
    return scratch;
}

Node parseJsonArg(FuncContext& ctx, const Value& value)
{
    if (value.type() == Type::Blob) {
        const Blob& blob = value.blob();
        const std::string_view bytes(reinterpret_cast<const char*>(blob.data()), blob.size());
        return JsonbParser(ctx, bytes).parse();
    }
    std::string scratch;
    const std::string text = textValue(value, scratch);
    return JsonParser(ctx, text, true).parse();
}

std::string realJson(double d)
{
    if (std::isnan(d))
        return {};
    if (std::isinf(d))
        return d < 0 ? "-9.0e+999" : "9.0e+999";
    std::string out;
    realToText(d, out);
    return out;
}

std::string valueSignature(const Value& value)
{
    std::string out(1, char('0' + int(value.type())));
    out.push_back(':');
    out += std::to_string(int(value.subtype()));
    out.push_back(':');
    switch (value.type()) {
        case Type::Null:
            break;
        case Type::Integer:
            out += std::to_string(value.integer());
            break;
        case Type::Real:
            out += realJson(value.real());
            break;
        case Type::Datetime:
            out += std::to_string(value.datetime().usec);
            break;
        case Type::Text:
            out += value.text();
            break;
        case Type::Blob: {
            const Blob& blob = value.blob();
            out.append(reinterpret_cast<const char*>(blob.data()), blob.size());
            break;
        }
    }
    return out;
}

Node valueToJson(FuncContext& ctx, const Value& value)
{
    Node n;
    switch (value.type()) {
        case Type::Null:
            n.kind = JKind::Null;
            n.jsonbType = 0;
            return n;
        case Type::Integer:
            n.kind = JKind::Integer;
            n.text = std::to_string(value.integer());
            n.raw = n.text;
            n.jsonbType = 3;
            return n;
        case Type::Real: {
            const std::string text = realJson(value.real());
            if (text.empty()) {
                n.kind = JKind::Null;
                n.jsonbType = 0;
            } else {
                n.kind = JKind::Real;
                n.text = text;
                n.raw = text;
                n.jsonbType = 5;
            }
            return n;
        }
        case Type::Datetime:
        case Type::Text:
            if (value.subtype() == kJsonSubtype && value.type() == Type::Text)
                return parseJsonArg(ctx, value);
            n.kind = JKind::Text;
            appendText(value, n.text);
            n.raw = n.text;
            n.jsonbType = 7;
            for (const unsigned char c : n.text) {
                if (c == '"' || c == '\\' || c < 0x20) {
                    n.raw.clear();
                    n.raw.reserve(n.text.size() + 8);
                    for (const unsigned char ch : n.text) {
                        switch (ch) {
                            case '"':
                                n.raw += "\\\"";
                                break;
                            case '\\':
                                n.raw += "\\\\";
                                break;
                            case '\n':
                                n.raw += "\\n";
                                break;
                            case '\r':
                                n.raw += "\\r";
                                break;
                            case '\t':
                                n.raw += "\\t";
                                break;
                            default:
                                if (ch < 0x20) {
                                    static constexpr char kHex[] = "0123456789abcdef";
                                    n.raw += "\\u00";
                                    n.raw.push_back(kHex[ch >> 4]);
                                    n.raw.push_back(kHex[ch & 0xf]);
                                } else {
                                    n.raw.push_back(char(ch));
                                }
                                break;
                        }
                    }
                    n.jsonbType = 8;
                    break;
                }
            }
            return n;
        case Type::Blob: {
            const Blob& blob = value.blob();
            const std::string_view bytes(reinterpret_cast<const char*>(blob.data()), blob.size());
            if (isValidJsonb(bytes))
                return JsonbParser(ctx, bytes).parse();
            ctx.error("JSON cannot hold BLOB values");
        }
    }
    return n;
}

Node valueToJsonForEdit(FuncContext& ctx, const Value& value)
{
    Node n = valueToJson(ctx, value);
    if (value.type() == Type::Real && std::isinf(value.real())) {
        n.kind = JKind::Real;
        n.text = value.real() < 0 ? "-9e999" : "9e999";
        n.raw = n.text;
        n.jsonbType = 5;
    } else if ((value.type() == Type::Text || value.type() == Type::Datetime) &&
               !(value.type() == Type::Text && value.subtype() == kJsonSubtype) &&
               n.kind == JKind::Text) {
        n.jsonbType = 10;
    }
    return n;
}

Node valueToJsonAggregate(FuncContext& ctx, const Value& value)
{
    if (value.type() == Type::Text && value.subtype() == kJsonSubtype) {
        Value copy = value;
        copy.setSubtype(0);
        return valueToJson(ctx, copy);
    }
    return valueToJson(ctx, value);
}

struct PathLeg
{
    enum class Type
    {
        Key,
        Index,
        FromEnd,
        Append,
    } type = Type::Key;
    std::string key;
    long long index = 0;
};

using Path = std::vector<PathLeg>;

Path parsePath(FuncContext& ctx, std::string_view path)
{
    if (path.empty() || path[0] != '$')
        ctx.error("bad JSON path: '" + std::string(path) + "'");
    Path out;
    std::size_t pos = 1;
    while (pos < path.size()) {
        if (path[pos] == '.') {
            ++pos;
            PathLeg leg;
            leg.type = PathLeg::Type::Key;
            if (pos < path.size() && path[pos] == '"') {
                std::size_t end = pos + 1;
                while (end < path.size() && path[end] != '"')
                    ++end;
                if (end >= path.size())
                    ctx.error("bad JSON path: '" + std::string(path) + "'");
                leg.key.assign(path.substr(pos + 1, end - pos - 1));
                pos = end + 1;
            } else {
                const std::size_t start = pos;
                while (pos < path.size() && path[pos] != '.' && path[pos] != '[')
                    ++pos;
                if (start == pos)
                    ctx.error("bad JSON path: '" + std::string(path) + "'");
                leg.key.assign(path.substr(start, pos - start));
            }
            out.push_back(std::move(leg));
        } else if (path[pos] == '[') {
            ++pos;
            PathLeg leg;
            if (pos < path.size() && path[pos] == '#') {
                ++pos;
                if (pos < path.size() && path[pos] == ']') {
                    ++pos;
                    leg.type = PathLeg::Type::Append;
                    out.push_back(leg);
                    continue;
                }
                if (pos < path.size() && path[pos] == '-') {
                    ++pos;
                    leg.type = PathLeg::Type::FromEnd;
                } else {
                    ctx.error("bad JSON path: '" + std::string(path) + "'");
                }
            } else {
                leg.type = PathLeg::Type::Index;
            }
            const std::size_t start = pos;
            while (pos < path.size() && std::isdigit(static_cast<unsigned char>(path[pos])))
                ++pos;
            if (start == pos || pos >= path.size() || path[pos] != ']')
                ctx.error("bad JSON path: '" + std::string(path) + "'");
            leg.index = std::strtoll(std::string(path.substr(start, pos - start)).c_str(), nullptr, 10);
            ++pos;
            out.push_back(leg);
        } else {
            ctx.error("bad JSON path: '" + std::string(path) + "'");
        }
    }
    return out;
}

const Node* findPath(const Node& root, const Path& path)
{
    const Node* cur = &root;
    for (const PathLeg& leg : path) {
        if (leg.type == PathLeg::Type::Key) {
            if (cur->kind != JKind::Object)
                return nullptr;
            auto found = std::find_if(cur->object.begin(), cur->object.end(),
                                      [&](const Member& m) { return m.key.text == leg.key; });
            if (found == cur->object.end())
                return nullptr;
            cur = &found->value;
        } else {
            if (cur->kind != JKind::Array || leg.type == PathLeg::Type::Append)
                return nullptr;
            long long idx = leg.index;
            if (leg.type == PathLeg::Type::FromEnd)
                idx = static_cast<long long>(cur->array.size()) - idx;
            if (idx < 0 || static_cast<std::size_t>(idx) >= cur->array.size())
                return nullptr;
            cur = &cur->array[std::size_t(idx)];
        }
    }
    return cur;
}

Node* findParent(Node& root, const Path& path, const PathLeg*& last)
{
    if (path.empty()) {
        last = nullptr;
        return &root;
    }
    Path parent(path.begin(), path.end() - 1);
    last = &path.back();
    return const_cast<Node*>(findPath(root, parent));
}

std::string typeName(const Node& n)
{
    switch (n.kind) {
        case JKind::Null:
            return "null";
        case JKind::True:
            return "true";
        case JKind::False:
            return "false";
        case JKind::Integer:
            return "integer";
        case JKind::Real:
            return "real";
        case JKind::Text:
            return "text";
        case JKind::Array:
            return "array";
        case JKind::Object:
            return "object";
    }
    return "null";
}

void nodeToSql(const Node& n, Value& out, bool jsonForContainers)
{
    switch (n.kind) {
        case JKind::Null:
            out.setNull();
            break;
        case JKind::True:
            out.setInteger(1);
            break;
        case JKind::False:
            out.setInteger(0);
            break;
        case JKind::Integer: {
            char* end = nullptr;
            const long long value = std::strtoll(n.text.c_str(), &end, 10);
            if (end != n.text.c_str() && *end == '\0')
                out.setInteger(value);
            else
                out.setReal(std::strtod(n.text.c_str(), nullptr));
            break;
        }
        case JKind::Real:
            out.setReal(std::strtod(n.text.c_str(), nullptr));
            break;
        case JKind::Text:
            out.setText(n.text);
            break;
        case JKind::Array:
        case JKind::Object:
            if (jsonForContainers)
                jsonResult(out, toJson(n));
            else
                out.setText(toJson(n));
            break;
    }
}

void setBlobResult(Value& out, const std::string& bytes)
{
    out.setBlob(bytes.data(), bytes.size());
}

void fnJson(FuncContext& ctx, int, Args argv, Value& out)
{
    jsonResult(out, toJson(parseJsonArg(ctx, *argv[0])));
}

void fnJsonb(FuncContext& ctx, int, Args argv, Value& out)
{
    setBlobResult(out, toJsonb(parseJsonArg(ctx, *argv[0])));
}

void fnJsonArray(FuncContext& ctx, int argc, Args argv, Value& out)
{
    Node n;
    n.kind = JKind::Array;
    n.jsonbType = 11;
    for (int i = 0; i < argc; ++i)
        n.array.push_back(valueToJson(ctx, *argv[i]));
    jsonResult(out, toJson(n));
}

void fnJsonbArray(FuncContext& ctx, int argc, Args argv, Value& out)
{
    Node n;
    n.kind = JKind::Array;
    n.jsonbType = 11;
    for (int i = 0; i < argc; ++i)
        n.array.push_back(valueToJson(ctx, *argv[i]));
    setBlobResult(out, toJsonb(n));
}

void fnJsonObject(FuncContext& ctx, int argc, Args argv, Value& out)
{
    if ((argc & 1) != 0)
        ctx.error("json_object() requires an even number of arguments");
    Node n;
    n.kind = JKind::Object;
    n.jsonbType = 12;
    for (int i = 0; i < argc; i += 2) {
        if (argv[i]->type() != Type::Text)
            ctx.error("json_object() labels must be TEXT");
        Member m;
        m.key.kind = JKind::Text;
        m.key.text = argv[i]->text();
        m.key.raw = m.key.text;
        m.key.jsonbType = 7;
        m.value = valueToJson(ctx, *argv[i + 1]);
        n.object.push_back(std::move(m));
    }
    jsonResult(out, toJson(n));
}

void fnJsonbObject(FuncContext& ctx, int argc, Args argv, Value& out)
{
    Value tmp;
    fnJsonObject(ctx, argc, argv, tmp);
    setBlobResult(out, toJsonb(parseJsonArg(ctx, tmp)));
}

void fnJsonQuote(FuncContext& ctx, int, Args argv, Value& out)
{
    if (argv[0]->type() == Type::Text && argv[0]->subtype() == kJsonSubtype) {
        jsonResult(out, argv[0]->text());
        return;
    }
    jsonResult(out, toJson(valueToJson(ctx, *argv[0])));
}

void fnJsonExtract(FuncContext& ctx, int argc, Args argv, Value& out)
{
    Node root = parseJsonArg(ctx, *argv[0]);
    if (argc == 1) {
        out.setNull();
        return;
    }
    if (argc == 2) {
        std::string scratch;
        const Node* n = findPath(root, parsePath(ctx, textValue(*argv[1], scratch)));
        if (n == nullptr) {
            out.setNull();
            return;
        }
        nodeToSql(*n, out, true);
        return;
    }
    Node array;
    array.kind = JKind::Array;
    array.jsonbType = 11;
    for (int i = 1; i < argc; ++i) {
        std::string scratch;
        const Node* n = findPath(root, parsePath(ctx, textValue(*argv[i], scratch)));
        array.array.push_back(n == nullptr ? Node{} : *n);
    }
    jsonResult(out, toJson(array));
}

void fnJsonbExtract(FuncContext& ctx, int argc, Args argv, Value& out)
{
    fnJsonExtract(ctx, argc, argv, out);
    if (out.type() == Type::Text && out.subtype() == kJsonSubtype)
        setBlobResult(out, toJsonb(parseJsonArg(ctx, out)));
}

void fnJsonArrayLength(FuncContext& ctx, int argc, Args argv, Value& out)
{
    Node root = parseJsonArg(ctx, *argv[0]);
    const Node* n = &root;
    if (argc == 2) {
        std::string scratch;
        n = findPath(root, parsePath(ctx, textValue(*argv[1], scratch)));
    }
    if (n == nullptr) {
        out.setNull();
    } else if (n->kind == JKind::Array) {
        out.setInteger(static_cast<std::int64_t>(n->array.size()));
    } else {
        out.setInteger(0);
    }
}

void fnJsonType(FuncContext& ctx, int argc, Args argv, Value& out)
{
    Node root = parseJsonArg(ctx, *argv[0]);
    const Node* n = &root;
    if (argc == 2) {
        std::string scratch;
        n = findPath(root, parsePath(ctx, textValue(*argv[1], scratch)));
    }
    if (n == nullptr)
        out.setNull();
    else
        out.setText(typeName(*n));
}

void fnJsonValid(FuncContext& ctx, int argc, Args argv, Value& out)
{
    if (argv[0]->type() == Type::Null) {
        out.setNull();
        return;
    }
    int flags = argc == 1 ? 1 : int(toInt64(*argv[1]));
    if (flags < 1 || flags > 15)
        ctx.error("FLAGS parameter to json_valid() must be between 1 and 15");
    bool valid = false;
    if (argv[0]->type() == Type::Blob) {
        const Blob& blob = argv[0]->blob();
        const std::string_view bytes(reinterpret_cast<const char*>(blob.data()), blob.size());
        valid = ((flags & 0x04) != 0 && isValidJsonb(bytes)) ||
                ((flags & 0x08) != 0 && JsonbParser(ctx, bytes).valid());
    } else {
        std::string scratch;
        const std::string text = textValue(*argv[0], scratch);
        if ((flags & 0x02) != 0) {
            try {
                (void)JsonParser(ctx, text, true).parse();
                valid = true;
            } catch (const Error&) {
                valid = false;
            }
        }
        if (!valid && (flags & 0x01) != 0) {
            try {
                (void)JsonParser(ctx, text, false).parse();
                valid = true;
            } catch (const Error&) {
                valid = false;
            }
        }
    }
    out.setInteger(valid ? 1 : 0);
}

void fnJsonErrorPosition(FuncContext& ctx, int, Args argv, Value& out)
{
    if (argv[0]->type() == Type::Blob) {
        const Blob& blob = argv[0]->blob();
        const std::string_view bytes(reinterpret_cast<const char*>(blob.data()), blob.size());
        out.setInteger(JsonbParser(ctx, bytes).valid() ? 0 : 1);
        return;
    }
    std::string scratch;
    const std::string text = textValue(*argv[0], scratch);
    out.setInteger(static_cast<std::int64_t>(JsonParser(ctx, text, true).errorOffset()));
}

enum class EditMode
{
    Insert,
    Replace,
    Set,
};

void applyEdit(Node& root, const Path& path, Node value, EditMode mode)
{
    const PathLeg* last = nullptr;
    Node* parent = findParent(root, path, last);
    if (last == nullptr) {
        if (mode != EditMode::Insert)
            root = std::move(value);
        return;
    }
    if (parent == nullptr)
        return;
    if (last->type == PathLeg::Type::Key) {
        if (parent->kind != JKind::Object)
            return;
        auto found = std::find_if(parent->object.begin(), parent->object.end(),
                                  [&](const Member& m) { return m.key.text == last->key; });
        if (found == parent->object.end()) {
            if (mode == EditMode::Replace)
                return;
            Member m;
            m.key.kind = JKind::Text;
            m.key.text = last->key;
            m.key.raw = last->key;
            m.key.jsonbType = 10;
            m.value = std::move(value);
            parent->object.push_back(std::move(m));
        } else if (mode != EditMode::Insert) {
            found->value = std::move(value);
        }
    } else if (parent->kind == JKind::Array) {
        long long idx = last->index;
        if (last->type == PathLeg::Type::Append) {
            if (mode != EditMode::Replace)
                parent->array.push_back(std::move(value));
            return;
        }
        if (last->type == PathLeg::Type::FromEnd)
            idx = static_cast<long long>(parent->array.size()) - idx;
        if (idx < 0 || static_cast<std::size_t>(idx) > parent->array.size())
            return;
        if (static_cast<std::size_t>(idx) == parent->array.size()) {
            if (mode != EditMode::Replace)
                parent->array.push_back(std::move(value));
        } else if (mode != EditMode::Insert) {
            parent->array[std::size_t(idx)] = std::move(value);
        }
    }
}

void editJson(FuncContext& ctx, int argc, Args argv, Value& out, EditMode mode, bool jsonb)
{
    if ((argc & 1) == 0)
        ctx.error("json_insert() needs an odd number of arguments");
    Node root = parseJsonArg(ctx, *argv[0]);
    for (int i = 1; i < argc; i += 2) {
        std::string scratch;
        Path path = parsePath(ctx, textValue(*argv[i], scratch));
        applyEdit(root, path, valueToJsonForEdit(ctx, *argv[i + 1]), mode);
    }
    if (jsonb)
        setBlobResult(out, toJsonb(root));
    else
        jsonResult(out, toJson(root));
}

void fnJsonInsert(FuncContext& ctx, int argc, Args argv, Value& out)
{
    editJson(ctx, argc, argv, out, EditMode::Insert, false);
}

void fnJsonbInsert(FuncContext& ctx, int argc, Args argv, Value& out)
{
    editJson(ctx, argc, argv, out, EditMode::Insert, true);
}

void fnJsonReplace(FuncContext& ctx, int argc, Args argv, Value& out)
{
    editJson(ctx, argc, argv, out, EditMode::Replace, false);
}

void fnJsonbReplace(FuncContext& ctx, int argc, Args argv, Value& out)
{
    editJson(ctx, argc, argv, out, EditMode::Replace, true);
}

void fnJsonSet(FuncContext& ctx, int argc, Args argv, Value& out)
{
    editJson(ctx, argc, argv, out, EditMode::Set, false);
}

void fnJsonbSet(FuncContext& ctx, int argc, Args argv, Value& out)
{
    editJson(ctx, argc, argv, out, EditMode::Set, true);
}

void removePath(Node& root, const Path& path)
{
    const PathLeg* last = nullptr;
    Node* parent = findParent(root, path, last);
    if (last == nullptr || parent == nullptr)
        return;
    if (last->type == PathLeg::Type::Key && parent->kind == JKind::Object) {
        parent->object.erase(std::remove_if(parent->object.begin(), parent->object.end(),
                                            [&](const Member& m) { return m.key.text == last->key; }),
                             parent->object.end());
    } else if (parent->kind == JKind::Array &&
               (last->type == PathLeg::Type::Index || last->type == PathLeg::Type::FromEnd)) {
        long long idx = last->index;
        if (last->type == PathLeg::Type::FromEnd)
            idx = static_cast<long long>(parent->array.size()) - idx;
        if (idx >= 0 && static_cast<std::size_t>(idx) < parent->array.size())
            parent->array.erase(parent->array.begin() + idx);
    }
}

void fnJsonRemove(FuncContext& ctx, int argc, Args argv, Value& out)
{
    Node root = parseJsonArg(ctx, *argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string scratch;
        removePath(root, parsePath(ctx, textValue(*argv[i], scratch)));
    }
    jsonResult(out, toJson(root));
}

void fnJsonbRemove(FuncContext& ctx, int argc, Args argv, Value& out)
{
    Value tmp;
    fnJsonRemove(ctx, argc, argv, tmp);
    setBlobResult(out, toJsonb(parseJsonArg(ctx, tmp)));
}

Node mergePatch(const Node& target, const Node& patch)
{
    if (patch.kind != JKind::Object)
        return patch;
    Node out = target.kind == JKind::Object ? target : Node{};
    out.kind = JKind::Object;
    out.jsonbType = 12;
    for (const Member& pm : patch.object) {
        auto found = std::find_if(out.object.begin(), out.object.end(),
                                  [&](const Member& m) { return m.key.text == pm.key.text; });
        if (pm.value.kind == JKind::Null) {
            if (found != out.object.end())
                out.object.erase(found);
        } else if (found == out.object.end()) {
            out.object.push_back(pm);
        } else {
            found->value = mergePatch(found->value, pm.value);
        }
    }
    return out;
}

void fnJsonPatch(FuncContext& ctx, int, Args argv, Value& out)
{
    jsonResult(out, toJson(mergePatch(parseJsonArg(ctx, *argv[0]), parseJsonArg(ctx, *argv[1]))));
}

void fnJsonbPatch(FuncContext& ctx, int argc, Args argv, Value& out)
{
    Value tmp;
    fnJsonPatch(ctx, argc, argv, tmp);
    setBlobResult(out, toJsonb(parseJsonArg(ctx, tmp)));
}

void fnJsonArrow(FuncContext& ctx, int argc, Args argv, Value& out)
{
    (void)argc;
    std::string scratch;
    std::string rhs = textValue(*argv[1], scratch);
    if (rhs.empty() || rhs[0] != '$') {
        if (argv[1]->type() == Type::Integer)
            rhs = "$[" + std::to_string(argv[1]->integer()) + "]";
        else
            rhs = "$." + rhs;
    }
    Node root = parseJsonArg(ctx, *argv[0]);
    const Node* found = findPath(root, parsePath(ctx, rhs));
    if (found == nullptr) {
        out.setNull();
        return;
    }
    jsonResult(out, toJson(*found));
}

void fnJsonArrowText(FuncContext& ctx, int argc, Args argv, Value& out)
{
    std::string scratch;
    std::string rhs = textValue(*argv[1], scratch);
    if (rhs.empty() || rhs[0] != '$') {
        if (argv[1]->type() == Type::Integer)
            rhs = "$[" + std::to_string(argv[1]->integer()) + "]";
        else
            rhs = "$." + rhs;
    }
    Value path(rhs);
    const Value* args[2] = {argv[0], &path};
    fnJsonExtract(ctx, argc, args, out);
    if (out.type() == Type::Text)
        out.setSubtype(0);
}

class JsonGroup final : public AggregateState
{
public:
    explicit JsonGroup(bool object, bool blob) : object_(object), blob_(blob) {}

    void step(FuncContext& ctx, int argc, Args argv) override
    {
        (void)argc;
        GroupRow row;
        if (object_) {
            if (argv[0]->type() != Type::Text)
                ctx.error("json_object() labels must be TEXT");
            Node key;
            key.kind = JKind::Text;
            key.text = argv[0]->text();
            key.raw = key.text;
            key.jsonbType = 7;
            row.nodes.push_back(std::move(key));
            row.nodes.push_back(valueToJsonAggregate(ctx, *argv[1]));
            row.signature = valueSignature(*argv[0]) + "\n" + valueSignature(*argv[1]);
        } else {
            row.nodes.push_back(valueToJsonAggregate(ctx, *argv[0]));
            row.signature = valueSignature(*argv[0]);
        }
        rows_.push_back(std::move(row));
    }

    void inverse(FuncContext&, int argc, Args argv) override
    {
        (void)argc;
        const std::string target = object_ ? valueSignature(*argv[0]) + "\n" + valueSignature(*argv[1])
                                           : valueSignature(*argv[0]);
        auto it = std::find_if(rows_.begin(), rows_.end(), [&](const GroupRow& row) {
            return row.signature == target;
        });
        if (it != rows_.end())
            rows_.erase(it);
    }

    void value(FuncContext&, Value& out) override
    {
        Node n;
        n.kind = object_ ? JKind::Object : JKind::Array;
        n.jsonbType = object_ ? 12 : 11;
        if (object_) {
            for (const GroupRow& row : rows_) {
                Member m;
                m.key = row.nodes[0];
                m.value = row.nodes[1];
                n.object.push_back(std::move(m));
            }
        } else {
            for (const GroupRow& row : rows_)
                n.array.push_back(row.nodes[0]);
        }
        if (blob_ && rows_.empty()) {
            out.setText(object_ ? "{}" : "[]");
            return;
        }
        if (blob_)
            setBlobResult(out, toJsonb(n));
        else
            jsonResult(out, toJson(n));
    }

    void reset() override { rows_.clear(); }

private:
    struct GroupRow
    {
        std::vector<Node> nodes;
        std::string signature;
    };

    bool object_ = false;
    bool blob_ = false;
    std::vector<GroupRow> rows_;
};

std::unique_ptr<AggregateState> makeJsonGroupArray()
{
    return std::make_unique<JsonGroup>(false, false);
}

std::unique_ptr<AggregateState> makeJsonbGroupArray()
{
    return std::make_unique<JsonGroup>(false, true);
}

std::unique_ptr<AggregateState> makeJsonGroupObject()
{
    return std::make_unique<JsonGroup>(true, false);
}

std::unique_ptr<AggregateState> makeJsonbGroupObject()
{
    return std::make_unique<JsonGroup>(true, true);
}

struct Row
{
    std::int64_t rowid = 0;
    std::int64_t id = 0;
    std::optional<std::int64_t> parent;
    Value key;
    const Node* node = nullptr;
    std::string fullkey;
    std::string path;
};

void assignIds(Node& n, std::size_t base = 0)
{
    std::string encoded;
    encodeJsonb(n, encoded);
    n.id = base;
    if (n.kind == JKind::Array) {
        std::size_t pos = 0;
        std::uint8_t type = 0;
        std::size_t payload = 0;
        std::size_t header = 0;
        (void)readHeader(encoded, 0, type, payload, header);
        pos = header;
        for (Node& child : n.array) {
            child.id = base + pos;
            assignIds(child, child.id);
            std::size_t childPayload = 0;
            std::size_t childHeader = 0;
            std::uint8_t childType = 0;
            (void)readHeader(encoded, pos, childType, childPayload, childHeader);
            pos += childHeader + childPayload;
        }
    } else if (n.kind == JKind::Object) {
        std::size_t pos = 0;
        std::uint8_t type = 0;
        std::size_t payload = 0;
        std::size_t header = 0;
        (void)readHeader(encoded, 0, type, payload, header);
        pos = header;
        for (Member& m : n.object) {
            m.id = base + pos;
            m.key.id = base + pos;
            std::size_t keyPayload = 0;
            std::size_t keyHeader = 0;
            std::uint8_t keyType = 0;
            (void)readHeader(encoded, pos, keyType, keyPayload, keyHeader);
            pos += keyHeader + keyPayload;
            assignIds(m.value, base + pos);
            std::size_t valuePayload = 0;
            std::size_t valueHeader = 0;
            std::uint8_t valueType = 0;
            (void)readHeader(encoded, pos, valueType, valuePayload, valueHeader);
            pos += valueHeader + valuePayload;
        }
    }
}

std::string quotePathKey(std::string_view key)
{
    bool simple = !key.empty();
    for (const char c : key) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            simple = false;
            break;
        }
    }
    if (simple)
        return "." + std::string(key);
    std::string out = ".\"";
    out += key;
    out.push_back('"');
    return out;
}

void collectTree(const Node& n, std::vector<Row>& rows, const Value& key, std::optional<std::int64_t> parent,
                 std::string fullkey, std::string path, bool recursive,
                 std::optional<std::int64_t> idOverride = std::nullopt)
{
    Row row;
    row.rowid = static_cast<std::int64_t>(rows.size());
    row.id = idOverride.value_or(static_cast<std::int64_t>(n.id));
    row.parent = parent;
    row.key = key;
    row.node = &n;
    row.fullkey = fullkey;
    row.path = path;
    rows.push_back(row);
    if (!recursive)
        return;
    if (n.kind == JKind::Array) {
        for (std::size_t i = 0; i < n.array.size(); ++i) {
            Value childKey(static_cast<std::int64_t>(i));
            collectTree(n.array[i], rows, childKey, row.id, fullkey + "[" + std::to_string(i) + "]",
                        fullkey, true);
        }
    } else if (n.kind == JKind::Object) {
        for (const Member& m : n.object) {
            Value childKey(m.key.text);
            collectTree(m.value, rows, childKey, row.id, fullkey + quotePathKey(m.key.text), fullkey,
                        true, static_cast<std::int64_t>(m.id));
        }
    }
}

class JsonCursor final : public TableFunctionCursor
{
public:
    explicit JsonCursor(bool tree) : tree_(tree) {}

    void open(FuncContext& ctx, int argc, Args argv) override
    {
        originalJson_ = *argv[0];
        if (argc == 2)
            originalRoot_ = *argv[1];
        else
            originalRoot_.setText("$");
        root_ = parseJsonArg(ctx, *argv[0]);
        assignIds(root_);
        const Node* start = &root_;
        std::string rootPath = "$";
        if (argc == 2) {
            std::string scratch;
            rootPath = textValue(*argv[1], scratch);
            start = findPath(root_, parsePath(ctx, rootPath));
        }
        rows_.clear();
        pos_ = 0;
        if (start == nullptr)
            return;
        if (tree_) {
            Value nullKey;
            collectTree(*start, rows_, nullKey, std::nullopt, rootPath, "$", true);
        } else if (start->kind == JKind::Array) {
            for (std::size_t i = 0; i < start->array.size(); ++i) {
                Value key(static_cast<std::int64_t>(i));
                collectTree(start->array[i], rows_, key, std::nullopt,
                            rootPath + "[" + std::to_string(i) + "]", rootPath, false);
            }
        } else if (start->kind == JKind::Object) {
            for (const Member& m : start->object) {
                Value key(m.key.text);
                collectTree(m.value, rows_, key, std::nullopt, rootPath + quotePathKey(m.key.text),
                            rootPath, false, static_cast<std::int64_t>(m.id));
            }
        } else {
            Value nullKey;
            collectTree(*start, rows_, nullKey, std::nullopt, rootPath, "$", false);
        }
    }

    bool eof() const override { return pos_ >= rows_.size(); }

    void next(FuncContext&) override
    {
        if (pos_ < rows_.size())
            ++pos_;
    }

    void column(FuncContext&, int i, Value& out) override
    {
        const Row& row = rows_[pos_];
        switch (i) {
            case 0:
                out = row.key;
                break;
            case 1:
                if (row.node->kind == JKind::Array || row.node->kind == JKind::Object)
                    out.setText(toJson(*row.node));
                else
                    nodeToSql(*row.node, out, false);
                break;
            case 2:
                out.setText(typeName(*row.node));
                break;
            case 3:
                if (row.node->kind == JKind::Array || row.node->kind == JKind::Object)
                    out.setNull();
                else
                    nodeToSql(*row.node, out, false);
                break;
            case 4:
                out.setInteger(row.id);
                break;
            case 5:
                if (row.parent)
                    out.setInteger(*row.parent);
                else
                    out.setNull();
                break;
            case 6:
                out.setText(row.fullkey);
                break;
            case 7:
                out.setText(row.path);
                break;
            case 8:
                out = originalJson_;
                break;
            case 9:
                out = originalRoot_;
                break;
            default:
                out.setNull();
                break;
        }
    }

    std::int64_t rowid() const override { return rows_[pos_].rowid; }

private:
    bool tree_ = false;
    Node root_;
    std::vector<Row> rows_;
    std::size_t pos_ = 0;
    Value originalJson_;
    Value originalRoot_;
};

std::unique_ptr<TableFunctionCursor> makeJsonEach()
{
    return std::make_unique<JsonCursor>(false);
}

std::unique_ptr<TableFunctionCursor> makeJsonTree()
{
    return std::make_unique<JsonCursor>(true);
}

void addScalar(FunctionRegistry& registry, const char* name, int minArgs, int maxArgs, ScalarFn fn)
{
    registry.add(FunctionDef{std::string(name), minArgs, maxArgs, kDeterministic | kSubtypeAware,
                             fn, nullptr});
}

void addAggregate(FunctionRegistry& registry, const char* name, int argc, AggregateFactory factory)
{
    registry.add(FunctionDef{std::string(name), argc, argc,
                             kDeterministic | kInvertible | kSubtypeAware, nullptr, factory});
}

void addJsonTable(FunctionRegistry& registry, const char* name,
                  std::unique_ptr<TableFunctionCursor> (*make)())
{
    registry.addTable(TableFunctionDef{
        std::string(name),
        {"key", "value", "type", "atom", "id", "parent", "fullkey", "path", "json", "root"},
        8,
        1,
        2,
        make,
    });
}

}  // namespace

void registerJsonFunctions(FunctionRegistry& r)
{
    addScalar(r, "json", 1, 1, fnJson);
    addScalar(r, "jsonb", 1, 1, fnJsonb);
    addScalar(r, "json_array", 0, -1, fnJsonArray);
    addScalar(r, "jsonb_array", 0, -1, fnJsonbArray);
    addScalar(r, "json_array_length", 1, 2, fnJsonArrayLength);
    addScalar(r, "json_error_position", 1, 1, fnJsonErrorPosition);
    addScalar(r, "json_extract", 1, -1, fnJsonExtract);
    addScalar(r, "jsonb_extract", 1, -1, fnJsonbExtract);
    addScalar(r, "json_insert", 1, -1, fnJsonInsert);
    addScalar(r, "jsonb_insert", 1, -1, fnJsonbInsert);
    addScalar(r, "json_object", 0, -1, fnJsonObject);
    addScalar(r, "jsonb_object", 0, -1, fnJsonbObject);
    addScalar(r, "json_patch", 2, 2, fnJsonPatch);
    addScalar(r, "jsonb_patch", 2, 2, fnJsonbPatch);
    addScalar(r, "json_quote", 1, 1, fnJsonQuote);
    addScalar(r, "json_remove", 1, -1, fnJsonRemove);
    addScalar(r, "jsonb_remove", 1, -1, fnJsonbRemove);
    addScalar(r, "json_replace", 1, -1, fnJsonReplace);
    addScalar(r, "jsonb_replace", 1, -1, fnJsonbReplace);
    addScalar(r, "json_set", 1, -1, fnJsonSet);
    addScalar(r, "jsonb_set", 1, -1, fnJsonbSet);
    addScalar(r, "json_type", 1, 2, fnJsonType);
    addScalar(r, "json_valid", 1, 2, fnJsonValid);
    addScalar(r, "->", 2, 2, fnJsonArrow);
    addScalar(r, "->>", 2, 2, fnJsonArrowText);

    addAggregate(r, "json_group_array", 1, makeJsonGroupArray);
    addAggregate(r, "jsonb_group_array", 1, makeJsonbGroupArray);
    addAggregate(r, "json_group_object", 2, makeJsonGroupObject);
    addAggregate(r, "jsonb_group_object", 2, makeJsonbGroupObject);

    addJsonTable(r, "json_each", makeJsonEach);
    addJsonTable(r, "json_tree", makeJsonTree);
}

}  // namespace sql::internal
