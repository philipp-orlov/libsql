// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The C interface (include/sql/sql_c.h): handles around the C++ objects, a status for every failure.
#include "sql/sql_c.h"

#include <chrono>
#include <exception>
#include <new>
#include <string>

#include "nosql/nosql.hpp"
#include "sql/sql.hpp"

struct sql_db
{
    sql::Database db;
};
struct sql_txn
{
    sql::Transaction txn;
};
struct sql_stmt
{
    sql::Statement stmt;
};
struct sql_result
{
    sql::Result result;
};
struct sql_table
{
    sql::TableInfo info;
};

namespace {

thread_local std::string lastError;
thread_local int lastConstraint = 0;

int fail(sql::ErrorCode code, sql::Constraint constraint, const char* what)
{
    lastError = what;
    lastConstraint = static_cast<int>(constraint);
    return -static_cast<int>(code);
}

template <class F>
int guarded(F&& fn) noexcept
{
    try {
        return fn();
    } catch (const sql::Error& e) {
        return fail(e.code(), e.constraint(), e.what());
    } catch (const nosql::Error& e) {
        return fail(e.code() == nosql::ErrorCode::Busy ? sql::ErrorCode::Busy : sql::ErrorCode::Internal, sql::Constraint::None, e.what());
    } catch (const std::bad_alloc&) {
        return fail(sql::ErrorCode::Internal, sql::Constraint::None, "out of memory");
    } catch (const std::exception& e) {
        return fail(sql::ErrorCode::InvalidArgument, sql::Constraint::None, e.what());
    } catch (...) {
        return fail(sql::ErrorCode::Internal, sql::Constraint::None, "unknown error");
    }
}

std::string_view view(const char* p, size_t n) { return std::string_view(p ? p : "", n); }

sql::Value toValue(const sql_value& v)
{
    switch (v.type) {
    case SQL_NULL: return sql::Value();
    case SQL_INTEGER: return sql::Value(v.i);
    case SQL_REAL: return sql::Value(v.d);
    case SQL_DATETIME: return sql::Value(sql::Datetime{v.i});
    case SQL_TEXT: return sql::Value::text(view(static_cast<const char*>(v.data), v.size));
    case SQL_BLOB: return sql::Value::blob(v.data, v.size);
    }
    throw sql::Error(sql::ErrorCode::InvalidArgument, "unknown value type");
}

sql::Params toParams(const sql_value* values, size_t n)
{
    sql::Params params;
    params.reserve(n);
    for (size_t i = 0; i < n; i++)
        params.push_back(toValue(values[i]));
    return params;
}

void fromValue(const sql::Value& v, sql_value* out) noexcept
{
    *out = sql_value{};
    out->type = static_cast<int32_t>(v.type());
    switch (v.type()) {
    case sql::Type::Null: break;
    case sql::Type::Integer: out->i = v.integer(); break;
    case sql::Type::Real: out->d = v.real(); break;
    case sql::Type::Datetime: out->i = v.datetime().usec; break;
    case sql::Type::Text: {
        auto t = v.text();
        out->data = t.data();
        out->size = t.size();
        break;
    }
    case sql::Type::Blob: {
        auto b = v.blob();
        out->data = b.data();
        out->size = b.size();
        break;
    }
    }
}

void give(const std::string& s, const char** p, size_t* n) noexcept
{
    *p = s.data();
    *n = s.size();
}

std::chrono::milliseconds ms(uint32_t n) { return std::chrono::milliseconds(n); }

}  // namespace

extern "C" {

const char* sql_version(void) { return sql::version(); }
const char* sql_last_error(void) { return lastError.c_str(); }
int sql_last_constraint(void) { return lastConstraint; }
int sql_complete(const char* text, size_t len) { return sql::complete(view(text, len)) ? 1 : 0; }

int sql_open(const char* path, const sql_options* o, sql_db** out)
{
    return guarded([&] {
        auto opts = sql::Database::configure();
        if (o) {
            if (o->max_size) opts.maxSize(o->max_size);
            if (o->buffer_cache) opts.bufferCache(o->buffer_cache);
            if (o->busy_timeout_ms) opts.busyTimeout(ms(o->busy_timeout_ms));
            if (o->sync_interval_ms) opts.syncInterval(ms(o->sync_interval_ms));
            switch (o->durability) {
            case SQL_DURABILITY_SAFE: opts.durability(nosql::Durability::Safe); break;
            case SQL_DURABILITY_NO_META_SYNC: opts.durability(nosql::Durability::NoMetaSync); break;
            case SQL_DURABILITY_NONE: opts.durability(nosql::Durability::None); break;
            case SQL_DURABILITY_LAZY: opts.durability(nosql::Durability::Lazy); break;
            default: break;
            }
            switch (o->memory_profile) {
            case SQL_PROFILE_COMPACT: opts.memoryProfile(sql::MemoryProfile::Compact); break;
            case SQL_PROFILE_THROUGHPUT: opts.memoryProfile(sql::MemoryProfile::Throughput); break;
            default: break;
            }
            opts.readOnly(o->read_only != 0);
            opts.createIfMissing(o->no_create == 0);
        }
        *out = new sql_db{opts.open(path)};
        return 0;
    });
}

int sql_connect(const char* url, const sql_remote_options* o, sql_db** out)
{
    return guarded([&] {
        sql::RemoteOptions opts;
        if (o) {
            if (o->token) opts.token(o->token);
            if (o->authorization) opts.authorization(o->authorization);
            if (o->ca_file) opts.caFile(o->ca_file);
            if (o->client_name) opts.clientName(o->client_name);
            if (o->connect_timeout_ms) opts.connectTimeout(ms(o->connect_timeout_ms));
            if (o->request_timeout_ms) opts.requestTimeout(ms(o->request_timeout_ms));
            if (o->ping_interval_ms) opts.pingInterval(ms(o->ping_interval_ms));
            opts.readOnly(o->read_only != 0);
            opts.insecureSkipVerify(o->insecure_skip_verify != 0);
            opts.reconnect(o->no_reconnect == 0);
        }
        *out = new sql_db{sql::Database::connect(url, opts)};
        return 0;
    });
}

void sql_close(sql_db* db)
{
    if (!db)
        return;
    try {
        db->db.close();
    } catch (...) {
    }
    delete db;
}

int sql_is_remote(sql_db* db) { return db->db.remote() ? 1 : 0; }

int sql_open_reader(sql_db* db, sql_db** out)
{
    return guarded([&] {
        *out = new sql_db{db->db.openReader()};
        return 0;
    });
}

int sql_exec(sql_db* db, const char* text, size_t len, const sql_value* params, size_t n, sql_result** out)
{
    return guarded([&] {
        *out = new sql_result{db->db.exec(view(text, len), toParams(params, n))};
        return 0;
    });
}

int sql_changes(sql_db* db, int64_t* out)
{
    return guarded([&] {
        *out = db->db.changes();
        return 0;
    });
}

int sql_total_changes(sql_db* db, int64_t* out)
{
    return guarded([&] {
        *out = db->db.totalChanges();
        return 0;
    });
}

int sql_last_insert_rowid(sql_db* db, int64_t* out)
{
    return guarded([&] {
        *out = db->db.lastInsertRowid();
        return 0;
    });
}

int sql_in_transaction(sql_db* db) { return db->db.inTransaction() ? 1 : 0; }

void sql_interrupt(sql_db* db)
{
    try {
        db->db.interrupt();
    } catch (...) {
    }
}

int sql_begin(sql_db* db, int read_only, sql_txn** out)
{
    return guarded([&] {
        *out = new sql_txn{read_only ? db->db.beginRead() : db->db.begin()};
        return 0;
    });
}

int sql_txn_exec(sql_txn* txn, const char* text, size_t len, const sql_value* params, size_t n, sql_result** out)
{
    return guarded([&] {
        *out = new sql_result{txn->txn.exec(view(text, len), toParams(params, n))};
        return 0;
    });
}

int sql_commit(sql_txn* txn)
{
    int rc = guarded([&] {
        txn->txn.commit();
        return 0;
    });
    delete txn;
    return rc;
}

void sql_rollback(sql_txn* txn)
{
    if (!txn)
        return;
    try {
        txn->txn.rollback();
    } catch (...) {
    }
    delete txn;
}

int sql_prepare(sql_db* db, const char* text, size_t len, sql_stmt** out)
{
    return guarded([&] {
        *out = new sql_stmt{db->db.prepare(view(text, len))};
        return 0;
    });
}

void sql_stmt_free(sql_stmt* stmt) { delete stmt; }

int sql_stmt_parameters(sql_stmt* stmt)
{
    return guarded([&] { return static_cast<int>(stmt->stmt.parameters()); });
}

int sql_stmt_parameter_index(sql_stmt* stmt, const char* name, size_t len)
{
    return guarded([&] { return static_cast<int>(stmt->stmt.parameterIndex(view(name, len))); });
}

int sql_stmt_parameter_name(sql_stmt* stmt, int index, const char** name, size_t* len)
{
    thread_local std::string held;
    return guarded([&] {
        held = stmt->stmt.parameterName(static_cast<size_t>(index));
        give(held, name, len);
        return 0;
    });
}

int sql_stmt_writes(sql_stmt* stmt)
{
    return guarded([&] { return stmt->stmt.writes() ? 1 : 0; });
}

int sql_stmt_bind(sql_stmt* stmt, int index, const sql_value* value)
{
    return guarded([&] {
        stmt->stmt.bind(static_cast<size_t>(index), toValue(*value));
        return 0;
    });
}

int sql_stmt_clear(sql_stmt* stmt)
{
    return guarded([&] {
        stmt->stmt.clear();
        return 0;
    });
}

int sql_stmt_exec(sql_stmt* stmt, sql_txn* txn, sql_result** out)
{
    return guarded([&] {
        *out = new sql_result{txn ? stmt->stmt.exec(txn->txn) : stmt->stmt.exec()};
        return 0;
    });
}

void sql_result_free(sql_result* r) { delete r; }
size_t sql_result_columns(sql_result* r) { return r->result.columns().size(); }

int sql_result_column_name(sql_result* r, size_t column, const char** name, size_t* len)
{
    const auto& cols = r->result.columns();
    if (column >= cols.size())
        return fail(sql::ErrorCode::InvalidArgument, sql::Constraint::None, "column out of range");
    give(cols[column], name, len);
    return 0;
}

size_t sql_result_rows(sql_result* r) { return r->result.size(); }

int sql_result_cell(sql_result* r, size_t row, size_t column, sql_value* out)
{
    if (row >= r->result.size() || column >= r->result[row].size())
        return fail(sql::ErrorCode::InvalidArgument, sql::Constraint::None, "cell out of range");
    fromValue(r->result[row][column], out);
    return 0;
}

int sql_result_row(sql_result* r, size_t row, sql_value* out, size_t capacity)
{
    if (row >= r->result.size())
        return fail(sql::ErrorCode::InvalidArgument, sql::Constraint::None, "row out of range");
    const sql::Row& cells = r->result[row];
    if (capacity < cells.size())
        return fail(sql::ErrorCode::InvalidArgument, sql::Constraint::None, "row does not fit the array");
    size_t i = 0;
    for (const sql::Value& v : cells)
        fromValue(v, out + i++);
    return 0;
}

int64_t sql_result_changes(sql_result* r) { return static_cast<int64_t>(r->result.changes()); }
int64_t sql_result_last_insert_id(sql_result* r) { return r->result.lastInsertId(); }

int sql_tables(sql_db* db, const char** names, size_t* len, size_t* count)
{
    thread_local std::string joined;
    return guarded([&] {
        joined.clear();
        auto list = db->db.tables();
        for (const std::string& s : list) {
            joined += s;
            joined += '\0';
        }
        give(joined, names, len);
        *count = list.size();
        return 0;
    });
}

int sql_table_open(sql_db* db, const char* name, size_t len, sql_table** out)
{
    return guarded([&] {
        *out = new sql_table{db->db.table(view(name, len))};
        return 0;
    });
}

void sql_table_free(sql_table* t) { delete t; }
int sql_table_columns(sql_table* t) { return static_cast<int>(t->info.columns.size()); }

int sql_table_column(sql_table* t, int index, const char** name, size_t* name_len, const char** declared, size_t* declared_len, int* flags)
{
    if (index < 0 || static_cast<size_t>(index) >= t->info.columns.size())
        return fail(sql::ErrorCode::InvalidArgument, sql::Constraint::None, "column out of range");
    const sql::ColumnInfo& c = t->info.columns[static_cast<size_t>(index)];
    give(c.name, name, name_len);
    give(c.declaredType, declared, declared_len);
    *flags = (c.primaryKey ? 1 : 0) | (c.notNull ? 2 : 0) | (c.unique ? 4 : 0) | (c.hasDefault ? 8 : 0);
    return 0;
}

int sql_table_sql(sql_table* t, const char** text, size_t* len)
{
    give(t->info.sql, text, len);
    return 0;
}

}  // extern "C"
