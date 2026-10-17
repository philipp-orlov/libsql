// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "conformance_util.hpp"

#include "sqlite3.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace conformance {
namespace {

Cell fromSqlite(sqlite3_stmt* stmt, int column)
{
    Cell out;
    switch (sqlite3_column_type(stmt, column)) {
    case SQLITE_NULL:
        out.type = CellType::Null;
        break;
    case SQLITE_INTEGER:
        out.type = CellType::Integer;
        out.integer = sqlite3_column_int64(stmt, column);
        break;
    case SQLITE_FLOAT:
        out.type = CellType::Real;
        out.real = sqlite3_column_double(stmt, column);
        break;
    case SQLITE_TEXT: {
        out.type = CellType::Text;
        const auto* text = sqlite3_column_text(stmt, column);
        const int bytes = sqlite3_column_bytes(stmt, column);
        out.text.assign(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
        break;
    }
    case SQLITE_BLOB: {
        out.type = CellType::Blob;
        const auto* data = static_cast<const unsigned char*>(sqlite3_column_blob(stmt, column));
        const int bytes = sqlite3_column_bytes(stmt, column);
        if (bytes > 0)
            out.blob.assign(data, data + bytes);
        break;
    }
    default:
        out.type = CellType::Null;
        break;
    }
    return out;
}

Cell fromLibsql(const sql::Value& value)
{
    Cell out;
    switch (value.type()) {
    case sql::Type::Null:
        out.type = CellType::Null;
        break;
    case sql::Type::Integer:
        out.type = CellType::Integer;
        out.integer = value.integer();
        break;
    case sql::Type::Real:
        out.type = CellType::Real;
        out.real = value.real();
        break;
    case sql::Type::Datetime:
        out.type = CellType::Datetime;
        out.integer = value.datetime().usec;
        break;
    case sql::Type::Text:
        out.type = CellType::Text;
        out.text = value.text();
        break;
    case sql::Type::Blob: {
        out.type = CellType::Blob;
        const sql::Blob& blob = value.blob();
        out.blob.reserve(blob.size());
        for (std::byte byte : blob)
            out.blob.push_back(static_cast<unsigned char>(byte));
        break;
    }
    }
    return out;
}

std::int64_t sqliteLikeInteger(const Cell& value)
{
    switch (value.type) {
    case CellType::Integer:
    case CellType::Datetime:
        return value.integer;
    case CellType::Real:
        if (std::isnan(value.real))
            return 0;
        if (value.real > static_cast<double>(std::numeric_limits<std::int64_t>::max()))
            return std::numeric_limits<std::int64_t>::max();
        if (value.real < static_cast<double>(std::numeric_limits<std::int64_t>::min()))
            return std::numeric_limits<std::int64_t>::min();
        return static_cast<std::int64_t>(value.real);
    case CellType::Text: {
        char* end = nullptr;
        errno = 0;
        const long long parsed = std::strtoll(value.text.c_str(), &end, 10);
        if (end == value.text.c_str() || errno == ERANGE)
            return 0;
        return static_cast<std::int64_t>(parsed);
    }
    case CellType::Blob:
    case CellType::Null:
        return 0;
    }
    return 0;
}

double sqliteLikeReal(const Cell& value)
{
    switch (value.type) {
    case CellType::Integer:
    case CellType::Datetime:
        return static_cast<double>(value.integer);
    case CellType::Real:
        return value.real;
    case CellType::Text: {
        char* end = nullptr;
        const double parsed = std::strtod(value.text.c_str(), &end);
        if (end == value.text.c_str())
            return 0.0;
        return parsed;
    }
    case CellType::Blob:
    case CellType::Null:
        return 0.0;
    }
    return 0.0;
}

std::string sqliteLikeText(const Cell& value)
{
    switch (value.type) {
    case CellType::Null:
        return "";
    case CellType::Integer:
    case CellType::Datetime:
        return std::to_string(value.integer);
    case CellType::Real: {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.15g", value.real);
        return std::string(buf);
    }
    case CellType::Text:
        return value.text;
    case CellType::Blob:
        return std::string(reinterpret_cast<const char*>(value.blob.data()), value.blob.size());
    }
    return "";
}

std::uint32_t leftRotate(std::uint32_t value, std::uint32_t count)
{
    return (value << count) | (value >> (32u - count));
}

std::uint32_t load32(const unsigned char* p)
{
    return (static_cast<std::uint32_t>(p[0])) | (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) |
           (static_cast<std::uint32_t>(p[3]) << 24u);
}

void store32(unsigned char* p, std::uint32_t value)
{
    p[0] = static_cast<unsigned char>(value & 0xffu);
    p[1] = static_cast<unsigned char>((value >> 8u) & 0xffu);
    p[2] = static_cast<unsigned char>((value >> 16u) & 0xffu);
    p[3] = static_cast<unsigned char>((value >> 24u) & 0xffu);
}

std::string trimLeftForScript(const std::string& s)
{
    std::size_t pos = 0;
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
        ++pos;
    return s.substr(pos);
}

std::string upperAscii(std::string text)
{
    for (char& ch : text)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return text;
}

std::vector<std::string> topLevelWords(const std::string& sql)
{
    std::vector<std::string> words;
    int depth = 0;
    for (std::size_t i = 0; i < sql.size();) {
        const unsigned char c = static_cast<unsigned char>(sql[i]);
        if (c == '\'') {
            ++i;
            while (i < sql.size()) {
                if (sql[i] == '\'' && i + 1 < sql.size() && sql[i + 1] == '\'') {
                    i += 2;
                } else if (sql[i] == '\'') {
                    ++i;
                    break;
                } else {
                    ++i;
                }
            }
        } else if (c == '"') {
            ++i;
            while (i < sql.size()) {
                if (sql[i] == '"' && i + 1 < sql.size() && sql[i + 1] == '"') {
                    i += 2;
                } else if (sql[i] == '"') {
                    ++i;
                    break;
                } else {
                    ++i;
                }
            }
        } else if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            i += 2;
            while (i < sql.size() && sql[i] != '\n')
                ++i;
        } else if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            i += 2;
            while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/'))
                ++i;
            if (i + 1 < sql.size())
                i += 2;
        } else if (c == '(') {
            ++depth;
            ++i;
        } else if (c == ')') {
            if (depth > 0)
                --depth;
            ++i;
        } else if (depth == 0 && (std::isalpha(c) || c == '_')) {
            const std::size_t start = i;
            while (i < sql.size()) {
                const unsigned char d = static_cast<unsigned char>(sql[i]);
                if (!std::isalnum(d) && d != '_')
                    break;
                ++i;
            }
            words.push_back(upperAscii(sql.substr(start, i - start)));
        } else {
            ++i;
        }
    }
    return words;
}

}  // namespace

SqliteDb::SqliteDb()
{
    if (sqlite3_open(":memory:", &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(msg);
    }
}

SqliteDb::SqliteDb(const std::filesystem::path& path)
{
    if (sqlite3_open(path.string().c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(msg);
    }
}

SqliteDb::SqliteDb(SqliteDb&& other) noexcept : db_(other.db_)
{
    other.db_ = nullptr;
}

SqliteDb& SqliteDb::operator=(SqliteDb&& other) noexcept
{
    if (this != &other) {
        sqlite3_close(db_);
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

SqliteDb::~SqliteDb()
{
    sqlite3_close(db_);
}

ExecResult SqliteDb::exec(const std::string& sql)
{
    ExecResult out;
    const char* current = sql.c_str();
    const char* end = sql.c_str() + sql.size();
    bool preparedAny = false;

    while (current < end) {
        while (current < end && std::isspace(static_cast<unsigned char>(*current)))
            ++current;
        if (current >= end)
            break;

        sqlite3_stmt* stmt = nullptr;
        const char* tail = nullptr;
        const int prepareRc =
            sqlite3_prepare_v2(db_, current, static_cast<int>(end - current), &stmt, &tail);
        if (prepareRc != SQLITE_OK) {
            out.ok = false;
            out.error = sqlite3_errmsg(db_);
            return out;
        }
        current = tail ? tail : end;
        if (!stmt)
            continue;

        preparedAny = true;
        QueryResult result;
        const int cols = sqlite3_column_count(stmt);
        result.columns.reserve(static_cast<std::size_t>(cols));
        for (int c = 0; c < cols; ++c)
            result.columns.emplace_back(sqlite3_column_name(stmt, c));

        for (;;) {
            const int stepRc = sqlite3_step(stmt);
            if (stepRc == SQLITE_ROW) {
                Row row;
                row.reserve(static_cast<std::size_t>(cols));
                for (int c = 0; c < cols; ++c)
                    row.push_back(fromSqlite(stmt, c));
                result.rows.push_back(std::move(row));
            } else if (stepRc == SQLITE_DONE) {
                break;
            } else {
                out.ok = false;
                out.error = sqlite3_errmsg(db_);
                sqlite3_finalize(stmt);
                return out;
            }
        }
        const int finalizeRc = sqlite3_finalize(stmt);
        if (finalizeRc != SQLITE_OK) {
            out.ok = false;
            out.error = sqlite3_errmsg(db_);
            return out;
        }
        out.result = std::move(result);
    }

    if (!preparedAny)
        out.result = QueryResult{};
    return out;
}

LibsqlDb::LibsqlDb(const std::filesystem::path& path)
    : db_(sql::Database::configure().durable(false).open(path))
{}

ExecResult LibsqlDb::exec(const std::string& text)
{
    ExecResult out;
    try {
        sql::Result result = db_.exec(text);
        out.result.columns = result.columns();
        out.result.rows.reserve(result.size());
        for (const sql::Row& sqlRow : result) {
            Row row;
            row.reserve(sqlRow.size());
            for (const sql::Value& value : sqlRow)
                row.push_back(fromLibsql(value));
            out.result.rows.push_back(std::move(row));
        }
    } catch (const std::exception& e) {
        out.ok = false;
        out.error = e.what();
    } catch (...) {
        out.ok = false;
        out.error = "non-standard exception";
    }
    return out;
}

std::string md5Hex(const std::string& input)
{
    static const std::array<std::uint32_t, 64> shifts = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static const std::array<std::uint32_t, 64> constants = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
        0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
        0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
        0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
        0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
        0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
        0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
        0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};

    std::vector<unsigned char> msg(input.begin(), input.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(msg.size()) * 8u;
    msg.push_back(0x80u);
    while ((msg.size() % 64u) != 56u)
        msg.push_back(0u);
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<unsigned char>((bits >> (8u * static_cast<unsigned>(i))) & 0xffu));

    std::uint32_t a0 = 0x67452301u;
    std::uint32_t b0 = 0xefcdab89u;
    std::uint32_t c0 = 0x98badcfeu;
    std::uint32_t d0 = 0x10325476u;

    for (std::size_t offset = 0; offset < msg.size(); offset += 64u) {
        std::array<std::uint32_t, 16> m{};
        for (std::size_t i = 0; i < m.size(); ++i)
            m[i] = load32(msg.data() + offset + i * 4u);

        std::uint32_t a = a0;
        std::uint32_t b = b0;
        std::uint32_t c = c0;
        std::uint32_t d = d0;

        for (std::uint32_t i = 0; i < 64u; ++i) {
            std::uint32_t f = 0;
            std::uint32_t g = 0;
            if (i < 16u) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32u) {
                f = (d & b) | ((~d) & c);
                g = (5u * i + 1u) % 16u;
            } else if (i < 48u) {
                f = b ^ c ^ d;
                g = (3u * i + 5u) % 16u;
            } else {
                f = c ^ (b | (~d));
                g = (7u * i) % 16u;
            }
            const std::uint32_t next = b + leftRotate(a + f + constants[i] + m[g], shifts[i]);
            a = d;
            d = c;
            c = b;
            b = next;
        }

        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }

    std::array<unsigned char, 16> digest{};
    store32(digest.data(), a0);
    store32(digest.data() + 4, b0);
    store32(digest.data() + 8, c0);
    store32(digest.data() + 12, d0);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char byte : digest)
        out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

std::string sltRender(const Cell& value, char type)
{
    if (value.type == CellType::Null)
        return "NULL";
    switch (type) {
    case 'I':
        return std::to_string(sqliteLikeInteger(value));
    case 'R': {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3f", sqliteLikeReal(value));
        return std::string(buf);
    }
    case 'T':
    default: {
        std::string text = sqliteLikeText(value);
        if (text.empty())
            return "(empty)";
        for (char& ch : text) {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (c < 0x20u || c > 0x7eu)
                ch = '@';
        }
        return text;
    }
    }
}

std::vector<std::string> sltRenderedValues(const QueryResult& result, const std::string& types)
{
    std::vector<std::string> values;
    values.reserve(result.rows.size() * types.size());
    for (const Row& row : result.rows) {
        for (std::size_t c = 0; c < types.size(); ++c) {
            if (c < row.size())
                values.push_back(sltRender(row[c], types[c]));
            else
                values.push_back("NULL");
        }
    }
    return values;
}

void applySltSort(std::vector<std::string>& values, const std::string& mode, std::size_t width)
{
    if (mode == "valuesort") {
        std::sort(values.begin(), values.end());
    } else if (mode == "rowsort" || mode == "nosort") {
        if (mode == "rowsort" && width != 0u) {
            std::vector<std::vector<std::string>> rows;
            for (std::size_t i = 0; i < values.size(); i += width) {
                rows.emplace_back(values.begin() + static_cast<std::ptrdiff_t>(i),
                                  values.begin() +
                                      static_cast<std::ptrdiff_t>(std::min(values.size(), i + width)));
            }
            std::sort(rows.begin(), rows.end());
            values.clear();
            for (const std::vector<std::string>& row : rows)
                values.insert(values.end(), row.begin(), row.end());
        }
    }
}

std::vector<std::string> sltExpectedShape(const std::vector<std::string>& values, int hashThreshold)
{
    if (hashThreshold >= 0 && values.size() > static_cast<std::size_t>(hashThreshold)) {
        std::string input;
        for (const std::string& value : values) {
            input += value;
            input += '\n';
        }
        return {std::to_string(values.size()) + " values hashing to " + md5Hex(input)};
    }
    return values;
}

bool sameCell(const Cell& a, const Cell& b)
{
    if (a.type != b.type)
        return false;
    switch (a.type) {
    case CellType::Null:
        return true;
    case CellType::Integer:
    case CellType::Datetime:
        return a.integer == b.integer;
    case CellType::Real:
        if (a.real == 0.0 && b.real == 0.0)
            return true;
        return std::memcmp(&a.real, &b.real, sizeof(double)) == 0;
    case CellType::Text:
        return a.text == b.text;
    case CellType::Blob:
        return a.blob == b.blob;
    }
    return false;
}

std::string cellDebug(const Cell& value)
{
    std::ostringstream out;
    switch (value.type) {
    case CellType::Null:
        out << "NULL";
        break;
    case CellType::Integer:
        out << "INTEGER(" << value.integer << ")";
        break;
    case CellType::Real:
        out << "REAL(" << std::setprecision(17) << value.real << ")";
        break;
    case CellType::Datetime:
        out << "DATETIME(" << value.integer << ")";
        break;
    case CellType::Text:
        out << "TEXT(" << value.text << ")";
        break;
    case CellType::Blob:
        out << "BLOB(x'" << hexBytes(value.blob.data(), value.blob.size()) << "')";
        break;
    }
    return out.str();
}

std::string rowDebug(const Row& row)
{
    std::string out;
    for (std::size_t i = 0; i < row.size(); ++i) {
        if (i)
            out += " | ";
        out += cellDebug(row[i]);
    }
    return out;
}

int compareCellForSort(const Cell& a, const Cell& b)
{
    if (a.type != b.type)
        return static_cast<int>(a.type) < static_cast<int>(b.type) ? -1 : 1;
    switch (a.type) {
    case CellType::Null:
        return 0;
    case CellType::Integer:
    case CellType::Datetime:
        return a.integer < b.integer ? -1 : (a.integer > b.integer ? 1 : 0);
    case CellType::Real:
        if (sameCell(a, b))
            return 0;
        return a.real < b.real ? -1 : 1;
    case CellType::Text:
        return a.text < b.text ? -1 : (a.text > b.text ? 1 : 0);
    case CellType::Blob:
        return a.blob < b.blob ? -1 : (a.blob > b.blob ? 1 : 0);
    }
    return 0;
}

bool rowLess(const Row& a, const Row& b)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int cmp = compareCellForSort(a[i], b[i]);
        if (cmp != 0)
            return cmp < 0;
    }
    return a.size() < b.size();
}

bool sqliteComplete(const std::string& sql)
{
    return sqlite3_complete(sql.c_str()) != 0;
}

std::vector<std::string> splitSqlScript(const std::string& script)
{
    std::vector<std::string> statements;
    std::string current;
    for (char ch : script) {
        current.push_back(ch);
        if (sqliteComplete(current)) {
            const std::string trimmed = trimLeftForScript(current);
            if (!trimmed.empty())
                statements.push_back(current);
            current.clear();
        }
    }
    const std::string tail = trimLeftForScript(current);
    if (!tail.empty())
        statements.push_back(current);
    return statements;
}

bool isTopLevelSelectWithoutOrderBy(const std::string& sql)
{
    const std::vector<std::string> words = topLevelWords(sql);
    if (words.empty() || words[0] != "SELECT")
        return false;
    for (std::size_t i = 0; i + 1 < words.size(); ++i) {
        if (words[i] == "ORDER" && words[i + 1] == "BY")
            return false;
    }
    return true;
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path.string());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void writeFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("cannot write " + path.string());
    out << text;
}

std::vector<std::filesystem::path> collectTestFiles(const std::vector<std::string>& inputs,
                                                    const std::string& extension)
{
    std::vector<std::filesystem::path> files;
    for (const std::string& input : inputs) {
        std::filesystem::path path(input);
        if (std::filesystem::is_directory(path)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                if (entry.is_regular_file() && entry.path().extension() == extension)
                    files.push_back(entry.path());
            }
        } else if (std::filesystem::is_regular_file(path)) {
            files.push_back(path);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::filesystem::path makeTempDir(const std::string& prefix)
{
    const auto base = std::filesystem::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
        const auto path = base / (prefix + "-" + std::to_string(std::time(nullptr)) + "-" +
                                  std::to_string(i));
        std::error_code ec;
        if (std::filesystem::create_directories(path, ec))
            return path;
    }
    throw std::runtime_error("cannot create temporary directory");
}

std::string jsonEscape(const std::string& text)
{
    std::ostringstream out;
    for (unsigned char c : text) {
        switch (c) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (c < 0x20u) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(c) << std::dec;
            } else {
                out << static_cast<char>(c);
            }
            break;
        }
    }
    return out.str();
}

std::string hexBytes(const unsigned char* data, std::size_t size)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i)
        out << std::setw(2) << static_cast<unsigned>(data[i]);
    return out.str();
}

bool wildcardMatch(const std::string& pattern, const std::string& text)
{
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string::npos;
    std::size_t match = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++match;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

}  // namespace conformance
