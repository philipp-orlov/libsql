#include "internal/func.hpp"
#include "internal/cnumeric.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using sql::Type;
using sql::Value;
using sql::internal::Args;
using sql::internal::FuncContext;
using sql::internal::FunctionRegistry;

namespace {

std::string fromHex(std::string_view hex)
{
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return 0;
        };
        out.push_back(char((digit(hex[i]) << 4) | digit(hex[i + 1])));
    }
    return out;
}

std::string toHex(std::string_view text)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(text.size() * 2);
    for (unsigned char c : text) {
        encoded += hex[c >> 4];
        encoded += hex[c & 0x0f];
    }
    return encoded;
}

std::vector<std::string> splitTabs(const std::string& line)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            parts.push_back(line.substr(start));
            return parts;
        }
        parts.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

Value parseArg(const std::string& encoded)
{
    if (encoded.size() < 2 || encoded[1] != ':')
        return Value::null();
    const std::string payload = encoded.substr(2);
    switch (encoded[0]) {
        case 'N': return Value::null();
        case 'I': return Value(std::strtoll(payload.c_str(), nullptr, 10));
        case 'R': return Value(sql::internal::parseDouble(payload.c_str(), nullptr));
        case 'T': return Value::text(fromHex(payload));
        default: return Value::null();
    }
}

std::string typeName(const Value& value)
{
    switch (value.type()) {
        case Type::Null: return "null";
        case Type::Integer: return "integer";
        case Type::Real: return "real";
        case Type::Text: return "text";
        case Type::Datetime: return "datetime";
        case Type::Blob: return "blob";
    }
    return "null";
}

bool runCase(const FunctionRegistry& registry, const std::vector<std::string>& parts, int lineNo)
{
    if (parts.size() < 3)
        return false;
    std::vector<Value> values;
    std::vector<const Value*> ptrs;
    for (std::size_t i = 3; i < parts.size(); ++i)
        values.push_back(parseArg(parts[i]));
    for (const Value& value : values)
        ptrs.push_back(&value);
    const auto* fn = registry.find(parts[0], static_cast<int>(values.size()));
    if (fn == nullptr || fn->scalar == nullptr) {
        std::cerr << "line " << lineNo << ": missing function " << parts[0] << "\n";
        return false;
    }
    FuncContext ctx;
    Value out;
    Args args = ptrs.empty() ? nullptr : ptrs.data();
    fn->scalar(ctx, static_cast<int>(values.size()), args, out);
    const std::string actualType = typeName(out);
    const std::string actualValue = out.type() == Type::Text ? toHex(out.text()) : "";
    if (actualType != parts[1] || actualValue != parts[2]) {
        std::cerr << "line " << lineNo << " " << parts[0] << " mismatch\n"
                  << "  expected " << parts[1] << " " << parts[2] << "\n"
                  << "  actual   " << actualType << " " << actualValue << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: test_printf_funcs printf_cases.tsv\n";
        return 2;
    }
    FunctionRegistry registry;
    sql::internal::registerPrintfFunctions(registry);
    std::ifstream in(argv[1]);
    std::string line;
    int total = 0;
    int failed = 0;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        ++total;
        if (!runCase(registry, splitTabs(line), total))
            ++failed;
    }
    if (failed != 0) {
        std::cerr << failed << " of " << total << " printf cases failed\n";
        return 1;
    }
    std::cout << total << " printf cases passed\n";
    return 0;
}
