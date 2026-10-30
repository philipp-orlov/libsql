// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "sql/sql.hpp"

struct sqlite3;

namespace conformance {

enum class CellType
{
    Null,
    Integer,
    Real,
    Datetime,
    Text,
    Blob,
};

struct Cell
{
    CellType type = CellType::Null;
    std::int64_t integer = 0;
    double real = 0.0;
    std::string text;
    std::vector<unsigned char> blob;
};

using Row = std::vector<Cell>;

struct QueryResult
{
    std::vector<std::string> columns;
    std::vector<Row> rows;
};

struct ExecResult
{
    bool ok = true;
    std::string error;
    QueryResult result;
};

class SqliteDb
{
public:
    SqliteDb();
    explicit SqliteDb(const std::filesystem::path& path);
    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;
    SqliteDb(SqliteDb&& other) noexcept;
    SqliteDb& operator=(SqliteDb&& other) noexcept;
    ~SqliteDb();

    ExecResult exec(const std::string& sql);
    sqlite3* handle() noexcept { return db_; }

private:
    sqlite3* db_ = nullptr;
};

class LibsqlDb
{
public:
    explicit LibsqlDb(const std::filesystem::path& path);
    ExecResult exec(const std::string& sql);

private:
    sql::Database db_;
};

std::string md5Hex(const std::string& input);

std::string sltRender(const Cell& value, char type);
std::vector<std::string> sltRenderedValues(const QueryResult& result, const std::string& types);
void applySltSort(std::vector<std::string>& values, const std::string& mode, std::size_t width);
std::vector<std::string> sltExpectedShape(const std::vector<std::string>& values, int hashThreshold);

bool sameCell(const Cell& a, const Cell& b);
std::string cellDebug(const Cell& value);
std::string rowDebug(const Row& row);
int compareCellForSort(const Cell& a, const Cell& b);
bool rowLess(const Row& a, const Row& b);

bool sqliteComplete(const std::string& sql);
std::vector<std::string> splitSqlScript(const std::string& script);
bool isTopLevelSelectWithoutOrderBy(const std::string& sql);

std::string readFile(const std::filesystem::path& path);
void writeFile(const std::filesystem::path& path, const std::string& text);
std::vector<std::filesystem::path> collectTestFiles(const std::vector<std::string>& inputs,
                                                    const std::string& extension);
std::filesystem::path makeTempDir(const std::string& prefix);
std::string jsonEscape(const std::string& text);
std::string hexBytes(const unsigned char* data, std::size_t size);
bool wildcardMatch(const std::string& pattern, const std::string& text);

}  // namespace conformance
