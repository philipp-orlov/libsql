// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

/* A C interface to libsql, for languages that call C (the C# binding in bindings/csharp uses it).
 *
 * Every function that can fail returns a status: >= 0 is success, < 0 is -(sql::ErrorCode). On a failure
 * sql_last_error() is the message and sql_last_constraint() the sql::Constraint (0 when there is none), for the
 * calling thread. A database may be local (sql_open) or remote (sql_connect: ws://, wss://, unix://); everything
 * below works on both.
 *
 * A result owns its cells: text and blob values point into it and are valid until sql_result_free. Handles are
 * opaque and freed by their own function. One thread at a time per handle; use sql_open_reader or a second
 * connection for concurrency. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define SQL_C_API __declspec(dllexport)
#else
#define SQL_C_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sql_db sql_db;
typedef struct sql_txn sql_txn;
typedef struct sql_stmt sql_stmt;
typedef struct sql_result sql_result;
typedef struct sql_table sql_table;

enum { SQL_NULL = 0, SQL_INTEGER = 1, SQL_REAL = 2, SQL_DATETIME = 3, SQL_TEXT = 4, SQL_BLOB = 5 };

/* A value going in or coming out. integer and datetime (microseconds since the epoch) use i, real uses d, text and
 * blob use data and size. */
typedef struct sql_value
{
    int32_t type;
    int32_t reserved;
    int64_t i;
    double d;
    const void* data;
    size_t size;
} sql_value;

enum { SQL_DURABILITY_DEFAULT = 0, SQL_DURABILITY_SAFE = 1, SQL_DURABILITY_NO_META_SYNC = 2, SQL_DURABILITY_NONE = 3, SQL_DURABILITY_LAZY = 4 };
enum { SQL_PROFILE_BALANCED = 0, SQL_PROFILE_COMPACT = 1, SQL_PROFILE_THROUGHPUT = 2 };

/* Zero the struct and set what you need; zero means the default. */
typedef struct sql_options
{
    uint64_t max_size;
    uint64_t buffer_cache;
    uint32_t busy_timeout_ms;
    uint32_t sync_interval_ms;
    int32_t durability; /* SQL_DURABILITY_* */
    int32_t memory_profile;
    uint8_t read_only;
    uint8_t no_create;
    uint8_t reserved[6];
} sql_options;

typedef struct sql_remote_options
{
    const char* token;         /* bearer token, or NULL */
    const char* authorization; /* a complete Authorization header value, or NULL */
    const char* ca_file;
    const char* client_name;
    uint32_t connect_timeout_ms;
    uint32_t request_timeout_ms; /* 0: none */
    uint32_t ping_interval_ms;
    uint8_t read_only;
    uint8_t insecure_skip_verify;
    uint8_t no_reconnect;
    uint8_t reserved[5];
} sql_remote_options;

SQL_C_API const char* sql_version(void);
SQL_C_API const char* sql_last_error(void);
SQL_C_API int sql_last_constraint(void);
SQL_C_API int sql_complete(const char* text, size_t len); /* 1 when the text is one or more whole statements */

SQL_C_API int sql_open(const char* path, const sql_options* options, sql_db** out);
SQL_C_API int sql_connect(const char* url, const sql_remote_options* options, sql_db** out);
SQL_C_API void sql_close(sql_db* db);
SQL_C_API int sql_is_remote(sql_db* db);
SQL_C_API int sql_open_reader(sql_db* db, sql_db** out); /* a handle for a reading thread, on the same store */

/* Runs one or more statements (the last one's rows are the result). params may be NULL. */
SQL_C_API int sql_exec(sql_db* db, const char* text, size_t len, const sql_value* params, size_t n, sql_result** out);
SQL_C_API int sql_changes(sql_db* db, int64_t* out);
SQL_C_API int sql_total_changes(sql_db* db, int64_t* out);
SQL_C_API int sql_last_insert_rowid(sql_db* db, int64_t* out);
SQL_C_API int sql_in_transaction(sql_db* db);
SQL_C_API void sql_interrupt(sql_db* db); /* safe to call from another thread */

/* Transactions. */
SQL_C_API int sql_begin(sql_db* db, int read_only, sql_txn** out);
SQL_C_API int sql_txn_exec(sql_txn* txn, const char* text, size_t len, const sql_value* params, size_t n, sql_result** out);
SQL_C_API int sql_commit(sql_txn* txn);   /* consumes the handle, success or not */
SQL_C_API void sql_rollback(sql_txn* txn); /* consumes the handle */

/* Prepared statements. Parameter indexes are 1-based. */
SQL_C_API int sql_prepare(sql_db* db, const char* text, size_t len, sql_stmt** out);
SQL_C_API void sql_stmt_free(sql_stmt* stmt);
SQL_C_API int sql_stmt_parameters(sql_stmt* stmt);
SQL_C_API int sql_stmt_parameter_index(sql_stmt* stmt, const char* name, size_t len); /* 0 when there is none */
SQL_C_API int sql_stmt_parameter_name(sql_stmt* stmt, int index, const char** name, size_t* len);
SQL_C_API int sql_stmt_writes(sql_stmt* stmt);
SQL_C_API int sql_stmt_bind(sql_stmt* stmt, int index, const sql_value* value);
SQL_C_API int sql_stmt_clear(sql_stmt* stmt);
/* txn may be NULL for a statement of its own. */
SQL_C_API int sql_stmt_exec(sql_stmt* stmt, sql_txn* txn, sql_result** out);

/* Results. Rows and columns are 0-based. */
SQL_C_API void sql_result_free(sql_result* result);
SQL_C_API size_t sql_result_columns(sql_result* result);
SQL_C_API int sql_result_column_name(sql_result* result, size_t column, const char** name, size_t* len);
SQL_C_API size_t sql_result_rows(sql_result* result);
SQL_C_API int sql_result_cell(sql_result* result, size_t row, size_t column, sql_value* out);
/* All the cells of a row at once, into an array of at least sql_result_columns() values. */
SQL_C_API int sql_result_row(sql_result* result, size_t row, sql_value* out, size_t capacity);
SQL_C_API int64_t sql_result_changes(sql_result* result);
SQL_C_API int64_t sql_result_last_insert_id(sql_result* result);

/* The catalog. */
/* Table names, NUL-separated, in a buffer valid until the next call here on this thread. */
SQL_C_API int sql_tables(sql_db* db, const char** names, size_t* len, size_t* count);
SQL_C_API int sql_table_open(sql_db* db, const char* name, size_t len, sql_table** out);
SQL_C_API void sql_table_free(sql_table* table);
SQL_C_API int sql_table_columns(sql_table* table);
/* flags: 1 primary key, 2 not null, 4 unique, 8 has default */
SQL_C_API int sql_table_column(sql_table* table, int index, const char** name, size_t* name_len, const char** declared, size_t* declared_len, int* flags);
SQL_C_API int sql_table_sql(sql_table* table, const char** text, size_t* len);

#ifdef __cplusplus
}
#endif
