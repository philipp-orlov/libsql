#include "internal/func.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using sql::Blob;
using sql::Error;
using sql::Value;
using sql::internal::Args;
using sql::internal::FuncContext;
using sql::internal::FunctionRegistry;

namespace {

const char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(std::string_view bytes)
{
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::uint32_t acc = 0;
    int bits = 0;
    for (const unsigned char c : bytes) {
        acc = (acc << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(kBase64Alphabet[(acc >> bits) & 0x3f]);
        }
    }
    if (bits > 0) {
        acc <<= 6 - bits;
        out.push_back(kBase64Alphabet[acc & 0x3f]);
    }
    while (out.size() % 4 != 0)
        out.push_back('=');
    return out;
}

int base64Value(char c) noexcept
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

std::string base64Decode(std::string_view text)
{
    std::string out;
    out.reserve((text.size() / 4) * 3);
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=')
            break;
        const int v = base64Value(c);
        if (v < 0)
            continue;
        acc = (acc << 6) | unsigned(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(char((acc >> bits) & 0xff));
        }
    }
    return out;
}

std::vector<std::string> split(std::string_view text, char sep)
{
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t next = text.find(sep, pos);
        if (next == std::string_view::npos) {
            out.emplace_back(text.substr(pos));
            break;
        }
        out.emplace_back(text.substr(pos, next - pos));
        pos = next + 1;
    }
    return out;
}

std::string bytesOfBlob(const Blob& blob)
{
    return std::string(reinterpret_cast<const char*>(blob.data()), blob.size());
}

std::string realPayload(double d)
{
    if (std::isnan(d))
        return "nan";
    if (std::isinf(d))
        return d < 0 ? "-inf" : "inf";
    std::ostringstream out;
    out.precision(17);
    out << d;
    return out.str();
}

Value valueFromSpec(const std::string& spec)
{
    const std::vector<std::string> parts = split(spec, ',');
    Value out;
    if (parts.size() < 3)
        throw std::runtime_error("bad value spec: " + spec);
    const char type = parts[0].empty() ? 'N' : parts[0][0];
    const int subtype = std::stoi(parts[1]);
    switch (type) {
        case 'N':
            out.setNull();
            break;
        case 'I':
            out.setInteger(std::stoll(parts[2]));
            break;
        case 'R':
            if (parts[2] == "inf")
                out.setReal(INFINITY);
            else if (parts[2] == "-inf")
                out.setReal(-INFINITY);
            else if (parts[2] == "nan")
                out.setReal(NAN);
            else
                out.setReal(std::strtod(parts[2].c_str(), nullptr));
            break;
        case 'T': {
            const std::string text = base64Decode(parts[2]);
            out.setText(text);
            break;
        }
        case 'B': {
            const std::string bytes = base64Decode(parts[2]);
            out.setBlob(bytes.data(), bytes.size());
            break;
        }
        default:
            throw std::runtime_error("bad type in spec: " + spec);
    }
    out.setSubtype(static_cast<std::uint8_t>(subtype));
    return out;
}

std::string specFromValue(const Value& value)
{
    const int subtype = int(value.subtype());
    switch (value.type()) {
        case sql::Type::Null:
            return "N," + std::to_string(subtype) + ",";
        case sql::Type::Integer:
            return "I," + std::to_string(subtype) + "," + std::to_string(value.integer());
        case sql::Type::Real:
            return "R," + std::to_string(subtype) + "," + realPayload(value.real());
        case sql::Type::Datetime:
            return "T," + std::to_string(subtype) + "," + base64Encode(value.toText());
        case sql::Type::Text:
            return "T," + std::to_string(subtype) + "," + base64Encode(value.text());
        case sql::Type::Blob:
            return "B," + std::to_string(subtype) + "," + base64Encode(bytesOfBlob(value.blob()));
    }
    return "N,0,";
}

std::string tableRowsToPayload(FuncContext& ctx, const sql::internal::TableFunctionDef& table,
                               const std::vector<Value>& args)
{
    std::vector<const Value*> ptrs;
    ptrs.reserve(args.size());
    for (const Value& arg : args)
        ptrs.push_back(&arg);
    std::unique_ptr<sql::internal::TableFunctionCursor> cursor = table.make();
    cursor->open(ctx, int(args.size()), ptrs.data());
    std::vector<std::string> rows;
    while (!cursor->eof()) {
        std::vector<std::string> fields;
        fields.push_back(std::to_string(cursor->rowid()));
        for (int i = 0; i < 10; ++i) {
            Value column;
            cursor->column(ctx, i, column);
            fields.push_back(specFromValue(column));
        }
        std::ostringstream row;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i != 0)
                row << '|';
            row << fields[i];
        }
        rows.push_back(row.str());
        cursor->next(ctx);
    }
    std::ostringstream all;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i != 0)
            all << '\n';
        all << rows[i];
    }
    return base64Encode(all.str());
}

bool runScalar(FuncContext& ctx, const FunctionRegistry& registry, const std::vector<std::string>& fields,
               std::string& actual, std::string& error)
{
    const std::string& name = fields[1];
    const int argc = std::stoi(fields[2]);
    std::vector<Value> args;
    args.reserve(std::size_t(argc));
    for (int i = 0; i < argc; ++i)
        args.push_back(valueFromSpec(fields[3 + i]));
    std::vector<const Value*> ptrs;
    ptrs.reserve(args.size());
    for (const Value& arg : args)
        ptrs.push_back(&arg);
    const sql::internal::FunctionDef* def = registry.find(name, argc);
    if (def == nullptr || def->scalar == nullptr)
        throw std::runtime_error("missing scalar: " + name);
    try {
        Value out;
        def->scalar(ctx, argc, ptrs.data(), out);
        actual = specFromValue(out);
        return true;
    } catch (const Error& exc) {
        error = exc.what();
        return false;
    }
}

bool runAggregate(FuncContext& ctx, const FunctionRegistry& registry, const std::vector<std::string>& fields,
                  std::string& actual, std::string& error)
{
    const std::string& name = fields[1];
    const int argc = std::stoi(fields[2]);
    const std::string rowText = base64Decode(fields[3]);
    const sql::internal::FunctionDef* def = registry.find(name, argc);
    if (def == nullptr || def->aggregate == nullptr)
        throw std::runtime_error("missing aggregate: " + name);
    std::unique_ptr<sql::internal::AggregateState> state = def->aggregate();
    try {
        if (!rowText.empty()) {
            for (const std::string& rowSpec : split(rowText, ';')) {
                if (rowSpec.empty())
                    continue;
                const std::vector<std::string> argSpecs = split(rowSpec, '|');
                std::vector<Value> rowArgs;
                rowArgs.reserve(argSpecs.size());
                for (const std::string& spec : argSpecs)
                    rowArgs.push_back(valueFromSpec(spec));
                std::vector<const Value*> ptrs;
                ptrs.reserve(rowArgs.size());
                for (const Value& arg : rowArgs)
                    ptrs.push_back(&arg);
                state->step(ctx, argc, ptrs.data());
            }
        }
        Value out;
        state->value(ctx, out);
        actual = specFromValue(out);
        return true;
    } catch (const Error& exc) {
        error = exc.what();
        return false;
    }
}

bool runTable(FuncContext& ctx, const FunctionRegistry& registry, const std::vector<std::string>& fields,
              std::string& actual, std::string& error)
{
    const std::string& name = fields[1];
    const int argc = std::stoi(fields[2]);
    std::vector<Value> args;
    args.reserve(std::size_t(argc));
    for (int i = 0; i < argc; ++i)
        args.push_back(valueFromSpec(fields[3 + i]));
    const sql::internal::TableFunctionDef* table = registry.findTable(name);
    if (table == nullptr)
        throw std::runtime_error("missing table: " + name);
    try {
        actual = tableRowsToPayload(ctx, *table, args);
        return true;
    } catch (const Error& exc) {
        error = exc.what();
        return false;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "json_cases.tsv";
    std::ifstream input(path);
    if (!input) {
        std::cerr << "cannot open " << path << '\n';
        return 2;
    }

    FunctionRegistry registry;
    sql::internal::registerJsonFunctions(registry);
    FuncContext ctx;

    int total = 0;
    int passed = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        ++total;
        const std::vector<std::string> fields = split(line, '\t');
        std::string actual;
        std::string error;
        bool ok = false;
        std::string expectedStatus;
        std::string expected;
        try {
            if (fields[0] == "S") {
                const int argcValue = std::stoi(fields[2]);
                expectedStatus = fields[3 + argcValue];
                expected = fields[4 + argcValue];
                ok = runScalar(ctx, registry, fields, actual, error);
            } else if (fields[0] == "A") {
                expectedStatus = fields[4];
                expected = fields[5];
                ok = runAggregate(ctx, registry, fields, actual, error);
            } else if (fields[0] == "T") {
                const int argcValue = std::stoi(fields[2]);
                expectedStatus = fields[3 + argcValue];
                expected = fields[4 + argcValue];
                ok = runTable(ctx, registry, fields, actual, error);
            } else {
                throw std::runtime_error("bad case kind");
            }
        } catch (const std::exception& exc) {
            std::cerr << "case " << total << " harness failure: " << exc.what() << '\n';
            std::cerr << line << '\n';
            return 2;
        }

        if (expectedStatus == "OK") {
            if (ok && actual == expected) {
                ++passed;
                continue;
            }
            std::cerr << "case " << total << " failed\nexpected OK " << expected << "\nactual "
                      << (ok ? actual : "ERR " + error) << '\n' << line << '\n';
            return 1;
        }

        const std::string expectedError = base64Decode(expected);
        if (!ok && error == expectedError) {
            ++passed;
            continue;
        }
        std::cerr << "case " << total << " failed\nexpected ERR " << expectedError << "\nactual "
                  << (ok ? "OK " + actual : "ERR " + error) << '\n' << line << '\n';
        return 1;
    }

    std::cout << "json function cases passed: " << passed << "/" << total << '\n';
    return passed == total ? 0 : 1;
}
