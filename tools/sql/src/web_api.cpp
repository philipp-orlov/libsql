// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "web_api.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "json/utf8.hpp"

namespace sqlweb {
namespace {

using json::Json;

// What a JavaScript number holds exactly.
constexpr std::int64_t kMaxSafeInteger = 9007199254740991;
constexpr std::size_t kBlobPreviewBytes = 32;
constexpr std::int64_t kDefaultMaxRows = 5000;
constexpr std::int64_t kMaxRowsLimit = 1000000;
constexpr std::int64_t kDefaultPageRows = 100;
constexpr std::int64_t kMaxPageRows = 1000;

class BadRequest : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class Conflict : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// ------------------------------------------------------------ requests ----

const Json& requireMember(const Json& request, std::string_view key)
{
    if (!request.isObject())
        throw BadRequest("the request body must be a JSON object");
    const Json* member = request.find(key);
    if (!member)
        throw BadRequest("missing \"" + std::string(key) + "\"");
    return *member;
}

std::string requireString(const Json& request, std::string_view key)
{
    const Json& member = requireMember(request, key);
    if (!member.isString())
        throw BadRequest("\"" + std::string(key) + "\" must be a string");
    return member.asString();
}

std::int64_t optionalInteger(const Json& request, std::string_view key, std::int64_t fallback,
                             std::int64_t low, std::int64_t high)
{
    const Json* member = request.isObject() ? request.find(key) : nullptr;
    if (!member || member->isNull())
        return fallback;
    long long value = 0;
    if (!member->isNumber() || !member->tryInt64(value))
        throw BadRequest("\"" + std::string(key) + "\" must be an integer");
    return std::clamp<std::int64_t>(value, low, high);
}

bool equalsNoCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

std::string trim(std::string_view text)
{
    std::size_t first = 0;
    std::size_t last = text.size();
    while (first < last && std::isspace(static_cast<unsigned char>(text[first])))
        ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])))
        --last;
    return std::string(text.substr(first, last - first));
}

// ---------------------------------------------------------------- cells ----

std::string hexOf(const unsigned char* bytes, std::size_t count)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 0x0F];
    }
    return out;
}

Json blobCell(const unsigned char* bytes, std::size_t size)
{
    return Json{{"type", "blob"},
                {"size", size},
                {"value", hexOf(bytes, std::min(size, kBlobPreviewBytes))}};
}

Json encodeValue(const sql::Value& value)
{
    switch (value.type()) {
        case sql::Type::Null: return Json();
        case sql::Type::Integer: {
            const std::int64_t i = value.integer();
            if (i >= -kMaxSafeInteger && i <= kMaxSafeInteger)
                return Json(static_cast<long long>(i));
            return Json{{"type", "integer"}, {"value", std::to_string(i)}};
        }
        case sql::Type::Real: {
            const double d = value.real();
            if (std::isfinite(d))
                return Json(d);
            return Json(std::isnan(d) ? "NaN" : (d > 0 ? "Inf" : "-Inf"));
        }
        case sql::Type::Datetime: return Json(sql::formatDatetime(value.datetime()));
        case sql::Type::Text: {
            const std::string_view text = value.text();
            if (json::isValidUtf8(text))
                return Json(text);
            return blobCell(reinterpret_cast<const unsigned char*>(text.data()), text.size());
        }
        case sql::Type::Blob: {
            const sql::BlobView blob = value.blob();
            return blobCell(reinterpret_cast<const unsigned char*>(blob.data()), blob.size());
        }
    }
    return Json();
}

sql::Value decodeValue(const Json& cell)
{
    switch (cell.type()) {
        case Json::Type::Null: return sql::Value();
        case Json::Type::Bool: return sql::Value(cell.asBool());
        case Json::Type::Number: {
            long long integer = 0;
            if (cell.isInteger() && cell.tryInt64(integer))
                return sql::Value(static_cast<std::int64_t>(integer));
            return sql::Value(cell.asDouble());
        }
        case Json::Type::String: return sql::Value(cell.asString());
        case Json::Type::Object: {
            const Json* type = cell.find("type");
            const Json* value = cell.find("value");
            if (type && value && type->isString() && value->isString() &&
                type->asString() == "integer") {
                const std::string& text = value->asString();
                std::int64_t integer = 0;
                const auto parsed =
                    std::from_chars(text.data(), text.data() + text.size(), integer);
                if (parsed.ec == std::errc() && parsed.ptr == text.data() + text.size())
                    return sql::Value(integer);
            }
            throw BadRequest("this cell cannot be sent back to the database");
        }
        default: break;
    }
    throw BadRequest("a cell must be null, a number, a string or an integer object");
}

// A filter text that reads as a number is compared as one, the way a typed column would.
sql::Value guessValue(const std::string& text)
{
    if (!text.empty() && (std::isdigit(static_cast<unsigned char>(text[0])) || text[0] == '-' ||
                          text[0] == '+' || text[0] == '.')) {
        std::int64_t integer = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), integer);
        if (parsed.ec == std::errc() && parsed.ptr == text.data() + text.size())
            return sql::Value(integer);
        char* end = nullptr;
        const double real = std::strtod(text.c_str(), &end);
        if (end == text.c_str() + text.size() && std::isfinite(real))
            return sql::Value(real);
    }
    return sql::Value(text);
}

// ---------------------------------------------------------------- SQL ----

std::string quote(const std::string& name)
{
    std::string out = "\"";
    for (const char c : name) {
        if (c == '"')
            out += '"';
        out += c;
    }
    return out + "\"";
}

/// True when the statement manages the transaction itself, or cannot run inside one.
bool controlsTransaction(std::string_view text)
{
    std::size_t i = 0;
    for (;;) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        if (text.substr(i, 2) == "--") {
            while (i < text.size() && text[i] != '\n')
                ++i;
        } else if (text.substr(i, 2) == "/*") {
            const std::size_t end = text.find("*/", i + 2);
            i = end == std::string_view::npos ? text.size() : end + 2;
        } else {
            break;
        }
    }
    std::size_t end = i;
    while (end < text.size() && std::isalpha(static_cast<unsigned char>(text[end])))
        ++end;
    const std::string_view word = text.substr(i, end - i);
    for (const char* keyword : {"BEGIN", "COMMIT", "END", "ROLLBACK", "SAVEPOINT", "RELEASE",
                                "PRAGMA", "VACUUM", "ATTACH", "DETACH"}) {
        if (equalsNoCase(word, keyword))
            return true;
    }
    return false;
}

/// Opens the pending transaction when there is none. A failure undoes only a transaction opened here.
template <class F>
auto inPendingTransaction(sql::Database& db, F&& work)
{
    const bool opened = !db.inTransaction();
    if (opened)
        db.exec("BEGIN");
    try {
        return work();
    } catch (...) {
        if (opened && db.inTransaction())
            db.exec("ROLLBACK");
        throw;
    }
}

std::int64_t schemaVersion(sql::Database& db)
{
    return db.exec("PRAGMA schema_version")[0][0].toInteger();
}

/// A table or view, as far as browsing and editing need to know it.
struct Relation
{
    std::string name;
    bool view = false;
    std::vector<sql::ColumnInfo> columns;
    /// SQL expressions that address one row: `rowid`, or the PRIMARY KEY of a WITHOUT ROWID table.
    /// Empty when a row cannot be addressed.
    std::vector<std::string> keys;

    const sql::ColumnInfo* column(std::string_view wanted) const
    {
        for (const sql::ColumnInfo& c : columns) {
            if (equalsNoCase(c.name, wanted))
                return &c;
        }
        return nullptr;
    }
};

std::vector<sql::ColumnInfo> viewColumns(sql::Database& db, const std::string& name)
{
    std::vector<sql::ColumnInfo> columns;
    for (const sql::Row& row : db.exec("PRAGMA table_info(" + quote(name) + ")")) {
        sql::ColumnInfo c;
        c.name = row["name"].toText();
        c.declaredType = row["type"].toText();
        c.notNull = row["notnull"].toInteger() != 0;
        columns.push_back(std::move(c));
    }
    return columns;
}

Relation describe(sql::Database& db, const std::string& wanted)
{
    const sql::Result found = db.exec(
        "SELECT type, name FROM sqlite_schema WHERE type IN ('table', 'view') AND name = ?1 "
        "COLLATE NOCASE",
        sql::Params{sql::Value(wanted)});
    if (found.empty())
        throw BadRequest("no such table: " + wanted);

    Relation relation;
    relation.name = found[0][1].toText();
    relation.view = found[0][0].toText() == "view";
    if (relation.view) {
        relation.columns = viewColumns(db, relation.name);
        return relation;
    }
    const sql::TableInfo info = db.table(relation.name);
    relation.columns = info.columns;
    if (info.withoutRowid) {
        for (const int position : info.primaryKey)
            relation.keys.push_back(quote(info.columns[static_cast<std::size_t>(position)].name));
    } else if (!relation.column("rowid")) {
        relation.keys.push_back("rowid");
    }
    return relation;
}

void requireEditable(const Relation& relation, bool readOnly)
{
    if (relation.view)
        throw BadRequest(relation.name + " is a view and cannot be edited");
    if (readOnly)
        throw BadRequest("the database is open read-only");
    if (relation.keys.empty())
        throw BadRequest("the rows of " + relation.name + " cannot be addressed");
}

/// `k1 = ?1 AND k2 = ?2` for the key expressions, with the key cells bound after `first` earlier parameters.
std::string keyCondition(const Relation& relation, std::size_t first)
{
    std::string out;
    for (std::size_t i = 0; i < relation.keys.size(); ++i) {
        if (i)
            out += " AND ";
        out += relation.keys[i] + " = ?" + std::to_string(first + i + 1);
    }
    return out;
}

void appendKey(const Relation& relation, const Json& key, sql::Params& params)
{
    if (!key.isArray() || key.asArray().size() != relation.keys.size())
        throw BadRequest("a row key must be an array of " + std::to_string(relation.keys.size()) +
                         " cells");
    for (const Json& cell : key.asArray())
        params.push_back(decodeValue(cell));
}

struct Condition
{
    std::string sql;
    sql::Params params;
};

/// `text` is what the user typed under a column heading: `>5`, `=abc`, `<>NULL`, or plain text to look for.
std::optional<Condition> filterCondition(const std::string& column, std::string_view typed)
{
    const std::string text = trim(typed);
    if (text.empty())
        return std::nullopt;
    const std::string id = quote(column);
    // Longer operators first, so that `<>` is not read as `<`.
    static const std::pair<const char*, const char*> operators[] = {
        {"<>", "<>"}, {"!=", "<>"}, {">=", ">="}, {"<=", "<="},
        {"==", "="},  {"=", "="},   {">", ">"},   {"<", "<"}};
    for (const auto& [prefix, sqlOperator] : operators) {
        const std::string_view lead = prefix;
        if (text.compare(0, lead.size(), lead) != 0)
            continue;
        const std::string value = trim(std::string_view(text).substr(lead.size()));
        const std::string_view op = sqlOperator;
        if (equalsNoCase(value, "NULL") && (op == "=" || op == "<>"))
            return Condition{id + (op == "=" ? " IS NULL" : " IS NOT NULL"), {}};
        return Condition{id + " " + std::string(op) + " ?", {guessValue(value)}};
    }
    return Condition{"instr(lower(CAST(" + id + " AS TEXT)), lower(?)) > 0", {sql::Value(text)}};
}

// ------------------------------------------------------------ replies ----

Reply failure(int status, const std::string& message, const char* code = nullptr)
{
    Json body{{"error", message}};
    if (code)
        body["code"] = code;
    return Reply{status, std::move(body)};
}

/// Runs `work`, which returns the JSON body, and turns whatever it throws into an error reply.
template <class F>
Reply guarded(sql::Database& db, F&& work)
{
    Reply reply;
    try {
        reply.body = work();
    } catch (const BadRequest& e) {
        reply = failure(400, e.what());
    } catch (const Conflict& e) {
        reply = failure(409, e.what());
    } catch (const sql::Error& e) {
        reply = failure(400, e.what(), sql::toString(e.code()));
    } catch (const std::exception& e) {
        reply = failure(500, e.what());
    }
    try {
        reply.body["inTransaction"] = db.inTransaction();
    } catch (const std::exception&) {}
    return reply;
}

Json columnJson(const sql::ColumnInfo& c)
{
    Json out{{"name", c.name},          {"type", c.declaredType},
             {"affinity", c.affinity},  {"primaryKey", c.primaryKey},
             {"notNull", c.notNull},    {"unique", c.unique},
             {"generated", c.generated}};
    out["defaultSql"] = c.hasDefault ? Json(c.defaultSql) : Json();
    return out;
}

// --------------------------------------------------------------- pragmas ----

struct PragmaChoice
{
    int value;
    const char* label;
};

// Where a pragma lives: in the database file, in the connection of this tool (lost when the tool
// exits, and not shared with any other process), or only reported by the store.
struct PragmaSpec
{
    const char* name;
    const char* kind;  // boolean, integer, choice or text
    bool readOnly;
    const char* scope;  // file, session or store
    const char* description;
    std::vector<PragmaChoice> choices;
};

// Only pragmas that change what libsql does are listed.
const std::vector<PragmaSpec>& pragmaSpecs()
{
    static const std::vector<PragmaSpec> specs = {
        {"foreign_keys", "boolean", false, "session", "Enforce FOREIGN KEY constraints.", {}},
        {"recursive_triggers", "boolean", false, "session", "Let a trigger fire itself.", {}},
        {"ignore_check_constraints",
         "boolean",
         false,
         "session",
         "Do not enforce CHECK constraints.",
         {}},
        {"temp_store",
         "choice",
         false,
         "session",
         "Where a large ORDER BY may put its sorted runs. Memory never writes a temporary file.",
         {{0, "Default"}, {1, "File"}, {2, "Memory"}}},
        {"busy_timeout",
         "integer",
         false,
         "session",
         "Milliseconds a write waits for another writer. 0 waits without a limit.",
         {}},
        {"user_version",
         "integer",
         false,
         "file",
         "A number the application keeps in the file, for example a schema version.",
         {}},
        {"application_id",
         "integer",
         false,
         "file",
         "A number that tells which application owns the file.",
         {}},
        {"journal_mode",
         "text",
         true,
         "store",
         "Always wal. libsql reports this name because SQLite tools ask for it. The store is "
         "copy-on-write: a "
         "commit writes new pages and then switches to them, so there is no journal and no WAL "
         "file.",
         {}},
        {"encoding", "text", true, "store", "Text is always UTF-8.", {}},
        {"page_size", "integer", true, "store", "Bytes in one page of the store.", {}},
        {"page_count", "integer", true, "store", "Pages in use.", {}},
        {"freelist_count", "integer", true, "store", "Pages free for reuse.", {}},
        {"schema_version", "integer", true, "store", "Counts the changes to the schema.", {}},
    };
    return specs;
}

}  // namespace

// ---------------------------------------------------------------- WebApi ----

WebApi::WebApi(sql::Database& db, bool readOnly) : db_(db), readOnly_(readOnly) {}

Reply WebApi::info()
{
    return guarded(db_, [&] {
        // The names the SQL editor offers beside the keywords.
        Json functions = Json::array();
        for (const sql::Row& row :
             db_.exec("SELECT DISTINCT name FROM pragma_function_list ORDER BY name"))
            functions.push_back(row[0].toText());
        return Json{{"path", db_.path().string()},
                    {"version", sql::version()},
                    {"readOnly", readOnly_},
                    {"functions", std::move(functions)}};
    });
}

// A script that rebuilds the schema and the rows, as `.dump` of the command line does.
Reply WebApi::dump()
{
    return guarded(db_, [&] {
        std::string script = "BEGIN TRANSACTION;\n";
        const sql::Result tables = db_.exec(
            "SELECT name, sql FROM sqlite_schema WHERE type = 'table' AND sql IS NOT NULL");
        for (const sql::Row& table : tables) {
            const std::string name = table[0].toText();
            script += table[1].toText() + ";\n";
            std::string columns;
            std::string values;
            for (const sql::ColumnInfo& c : db_.table(name).columns) {
                if (c.generated)
                    continue;
                columns += (columns.empty() ? "" : ",") + quote(c.name);
                values +=
                    (values.empty() ? "" : "||','||") + std::string("quote(") + quote(c.name) + ")";
            }
            if (columns.empty())
                continue;
            const std::string prefix = "INSERT INTO " + quote(name) + "(" + columns + ") VALUES(";
            for (const sql::Row& row : db_.exec("SELECT " + values + " FROM " + quote(name)))
                script += prefix + row[0].toText() + ");\n";
        }
        for (const sql::Row& row :
             db_.exec("SELECT sql FROM sqlite_schema WHERE type <> 'table' AND sql IS NOT NULL"))
            script += row[0].toText() + ";\n";
        script += "COMMIT;\n";
        return Json{{"sql", std::move(script)}};
    });
}

Reply WebApi::schema()
{
    return guarded(db_, [&] {
        Json tables = Json::array();
        Json views = Json::array();
        Json triggers = Json::array();
        const sql::Result objects =
            db_.exec("SELECT type, name, tbl_name, sql FROM sqlite_schema WHERE sql IS NOT NULL");
        for (const sql::Row& row : objects) {
            const std::string type = row[0].toText();
            const std::string name = row[1].toText();
            if (type == "table") {
                const sql::TableInfo info = db_.table(name);
                Json columns = Json::array();
                for (const sql::ColumnInfo& c : info.columns)
                    columns.push_back(columnJson(c));
                tables.push_back(Json{{"name", name},
                                      {"sql", row[3].toText()},
                                      {"strict", info.strict},
                                      {"withoutRowid", info.withoutRowid},
                                      {"columns", std::move(columns)}});
            } else if (type == "view") {
                Json columns = Json::array();
                for (const sql::ColumnInfo& c : viewColumns(db_, name))
                    columns.push_back(columnJson(c));
                views.push_back(Json{
                    {"name", name}, {"sql", row[3].toText()}, {"columns", std::move(columns)}});
            } else if (type == "trigger") {
                triggers.push_back(
                    Json{{"name", name}, {"table", row[2].toText()}, {"sql", row[3].toText()}});
            }
        }
        Json indexes = Json::array();
        for (const sql::IndexInfo& index : db_.indexes()) {
            if (index.sql.empty())
                continue;
            Json columns = Json::array();
            for (const std::string& column : index.columns)
                columns.push_back(column);
            indexes.push_back(Json{{"name", index.name},
                                   {"table", index.table},
                                   {"unique", index.unique},
                                   {"partial", index.partial},
                                   {"sql", index.sql},
                                   {"columns", std::move(columns)}});
        }
        return Json{{"tables", std::move(tables)},
                    {"views", std::move(views)},
                    {"indexes", std::move(indexes)},
                    {"triggers", std::move(triggers)}};
    });
}

Reply WebApi::execute(const Json& request)
{
    return guarded(db_, [&] {
        const std::string text = requireString(request, "sql");
        const std::int64_t maxRows =
            optionalInteger(request, "maxRows", kDefaultMaxRows, 1, kMaxRowsLimit);

        // Like the "Execute SQL" tab of DB Browser: whatever the statements change stays pending until
        // commit() or revert(); a run that changed nothing leaves nothing pending.
        const bool managed = !db_.inTransaction() && !controlsTransaction(text);
        const std::int64_t changesBefore = db_.totalChanges();
        const std::int64_t schemaBefore = managed ? schemaVersion(db_) : 0;
        if (managed)
            db_.exec("BEGIN");

        const auto started = std::chrono::steady_clock::now();
        sql::Result result;
        try {
            result = db_.exec(text);
        } catch (...) {
            if (managed && db_.inTransaction())
                db_.exec("ROLLBACK");
            throw;
        }
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        if (managed && db_.inTransaction() && db_.totalChanges() == changesBefore &&
            schemaVersion(db_) == schemaBefore)
            db_.exec("ROLLBACK");

        Json columns = Json::array();
        for (const std::string& name : result.columns())
            columns.push_back(name);
        Json rows = Json::array();
        bool truncated = false;
        for (const sql::Row& row : result) {
            if (static_cast<std::int64_t>(rows.asArray().size()) >= maxRows) {
                truncated = true;
                break;
            }
            Json cells = Json::array();
            for (const sql::Value& value : row)
                cells.push_back(encodeValue(value));
            rows.push_back(std::move(cells));
        }
        return Json{{"columns", std::move(columns)}, {"rows", std::move(rows)},
                    {"rowCount", result.size()},     {"truncated", truncated},
                    {"changes", result.changes()},   {"lastInsertId", result.lastInsertId()},
                    {"elapsedMs", elapsedMs}};
    });
}

Reply WebApi::browse(const Json& request)
{
    return guarded(db_, [&] {
        const Relation relation = describe(db_, requireString(request, "table"));
        const std::int64_t offset =
            optionalInteger(request, "offset", 0, 0, std::numeric_limits<std::int32_t>::max());
        const std::int64_t limit =
            optionalInteger(request, "limit", kDefaultPageRows, 1, kMaxPageRows);

        std::string where;
        sql::Params params;
        if (const Json* filters = request.find("filters"); filters && filters->isObject()) {
            for (const auto& [name, text] : filters->asObject()) {
                const sql::ColumnInfo* column = relation.column(name);
                if (!column)
                    throw BadRequest("no such column: " + name);
                if (!text.isString())
                    throw BadRequest("a filter must be a string");
                if (auto condition = filterCondition(column->name, text.asString())) {
                    where += where.empty() ? " WHERE " : " AND ";
                    where += condition->sql;
                    for (sql::Value& value : condition->params)
                        params.push_back(std::move(value));
                }
            }
        }

        std::string order;
        if (const Json* sort = request.find("sort"); sort && sort->isArray()) {
            for (const Json& entry : sort->asArray()) {
                const sql::ColumnInfo* column = relation.column(requireString(entry, "column"));
                if (!column)
                    throw BadRequest("no such column: " + requireString(entry, "column"));
                const Json* descending = entry.find("descending");
                order += order.empty() ? " ORDER BY " : ", ";
                order +=
                    quote(column->name) +
                    (descending && descending->isBool() && descending->asBool() ? " DESC" : "");
            }
        }

        std::string select = "SELECT ";
        for (const std::string& key : relation.keys)
            select += key + ", ";
        for (std::size_t i = 0; i < relation.columns.size(); ++i)
            select += (i ? ", " : "") + quote(relation.columns[i].name);
        select += " FROM " + quote(relation.name) + where + order + " LIMIT " +
                  std::to_string(limit) + " OFFSET " + std::to_string(offset);

        const std::int64_t total =
            db_.exec("SELECT count(*) FROM " + quote(relation.name) + where, params)[0][0]
                .toInteger();
        const sql::Result page = db_.exec(select, params);

        const std::size_t keyCount = relation.keys.size();
        Json rows = Json::array();
        Json keys = Json::array();
        for (const sql::Row& row : page) {
            Json key = Json::array();
            Json cells = Json::array();
            for (std::size_t i = 0; i < row.size(); ++i)
                (i < keyCount ? key : cells).push_back(encodeValue(row[i]));
            keys.push_back(std::move(key));
            rows.push_back(std::move(cells));
        }
        Json columns = Json::array();
        for (const sql::ColumnInfo& c : relation.columns)
            columns.push_back(columnJson(c));
        return Json{{"table", relation.name},
                    {"view", relation.view},
                    {"editable", !relation.view && !readOnly_ && keyCount > 0},
                    {"columns", std::move(columns)},
                    {"rows", std::move(rows)},
                    {"keys", std::move(keys)},
                    {"total", total},
                    {"offset", offset},
                    {"limit", limit}};
    });
}

Reply WebApi::insertRow(const Json& request)
{
    return guarded(db_, [&] {
        const Relation relation = describe(db_, requireString(request, "table"));
        requireEditable(relation, readOnly_);
        std::string names;
        std::string marks;
        sql::Params params;
        if (const Json* values = request.find("values"); values && values->isObject()) {
            for (const auto& [name, cell] : values->asObject()) {
                const sql::ColumnInfo* column = relation.column(name);
                if (!column)
                    throw BadRequest("no such column: " + name);
                names += (names.empty() ? "" : ", ") + quote(column->name);
                params.push_back(decodeValue(cell));
                marks += marks.empty() ? "?" : ", ?";
            }
        }
        const std::string text =
            "INSERT INTO " + quote(relation.name) +
            (names.empty() ? " DEFAULT VALUES" : " (" + names + ") VALUES (" + marks + ")");
        const sql::Result result =
            inPendingTransaction(db_, [&] { return db_.exec(text, params); });
        return Json{{"changes", result.changes()}, {"lastInsertId", result.lastInsertId()}};
    });
}

Reply WebApi::updateRow(const Json& request)
{
    return guarded(db_, [&] {
        const Relation relation = describe(db_, requireString(request, "table"));
        requireEditable(relation, readOnly_);
        const Json& set = requireMember(request, "set");
        if (!set.isObject() || set.asObject().empty())
            throw BadRequest("\"set\" must name at least one column");

        std::string assignments;
        sql::Params params;
        for (const auto& [name, cell] : set.asObject()) {
            const sql::ColumnInfo* column = relation.column(name);
            if (!column)
                throw BadRequest("no such column: " + name);
            params.push_back(decodeValue(cell));
            assignments += (assignments.empty() ? "" : ", ") + quote(column->name) + " = ?" +
                           std::to_string(params.size());
        }
        const std::size_t assigned = params.size();
        appendKey(relation, requireMember(request, "key"), params);
        const std::string text = "UPDATE " + quote(relation.name) + " SET " + assignments +
                                 " WHERE " + keyCondition(relation, assigned);
        const sql::Result result = inPendingTransaction(db_, [&] {
            sql::Result done = db_.exec(text, params);
            if (done.changes() != 1)
                throw BadRequest("the row no longer exists");
            return done;
        });
        return Json{{"changes", result.changes()}};
    });
}

Reply WebApi::deleteRows(const Json& request)
{
    return guarded(db_, [&] {
        const Relation relation = describe(db_, requireString(request, "table"));
        requireEditable(relation, readOnly_);
        const Json& keys = requireMember(request, "keys");
        if (!keys.isArray())
            throw BadRequest("\"keys\" must be an array");
        const std::string text =
            "DELETE FROM " + quote(relation.name) + " WHERE " + keyCondition(relation, 0);
        const std::uint64_t changes = inPendingTransaction(db_, [&] {
            std::uint64_t total = 0;
            for (const Json& key : keys.asArray()) {
                sql::Params params;
                appendKey(relation, key, params);
                total += db_.exec(text, params).changes();
            }
            return total;
        });
        return Json{{"changes", changes}};
    });
}

Reply WebApi::commit()
{
    return guarded(db_, [&] {
        if (db_.inTransaction())
            db_.exec("COMMIT");
        return Json::object();
    });
}

Reply WebApi::revert()
{
    return guarded(db_, [&] {
        if (db_.inTransaction())
            db_.exec("ROLLBACK");
        return Json::object();
    });
}

Reply WebApi::pragmas()
{
    return guarded(db_, [&] {
        Json list = Json::array();
        for (const PragmaSpec& spec : pragmaSpecs()) {
            const sql::Result result = db_.exec(std::string("PRAGMA ") + spec.name);
            if (result.empty())
                continue;
            Json choices = Json::array();
            for (const PragmaChoice& choice : spec.choices)
                choices.push_back(Json{{"value", choice.value}, {"label", choice.label}});
            list.push_back(Json{{"name", spec.name},
                                {"kind", spec.kind},
                                {"scope", spec.scope},
                                {"description", spec.description},
                                {"readOnly", spec.readOnly || readOnly_},
                                {"value", encodeValue(result[0][0])},
                                {"choices", std::move(choices)}});
        }
        return Json{{"pragmas", std::move(list)}};
    });
}

Reply WebApi::setPragma(const Json& request)
{
    return guarded(db_, [&] {
        const std::string name = requireString(request, "name");
        const auto spec = std::find_if(pragmaSpecs().begin(), pragmaSpecs().end(),
                                       [&](const PragmaSpec& s) { return name == s.name; });
        if (spec == pragmaSpecs().end() || spec->readOnly)
            throw BadRequest("PRAGMA " + name + " cannot be changed here");
        if (readOnly_)
            throw BadRequest("the database is open read-only");
        if (db_.inTransaction())
            throw Conflict("write or revert the pending changes before changing a PRAGMA");

        const sql::Value value = decodeValue(requireMember(request, "value"));
        if (value.type() != sql::Type::Integer)
            throw BadRequest("the value of PRAGMA " + name + " must be an integer");
        const std::string kind = spec->kind;
        if (kind == "boolean" && value.integer() != 0 && value.integer() != 1)
            throw BadRequest("PRAGMA " + name + " takes 0 or 1");
        if (kind == "choice" &&
            std::none_of(spec->choices.begin(), spec->choices.end(),
                         [&](const PragmaChoice& c) { return c.value == value.integer(); }))
            throw BadRequest("PRAGMA " + name + " does not take " +
                             std::to_string(value.integer()));

        db_.exec("PRAGMA " + name + " = " + std::to_string(value.integer()));
        return Json::object();
    });
}

}  // namespace sqlweb
