// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The JSON API a browser front end uses to browse and change one sql::Database.
//
// Every method takes and returns plain JSON and never throws: a failure becomes a Reply with a 4xx or 5xx
// status and {"error": "..."}. The class knows nothing about HTTP, so it can be tested without a socket.
//
// Cells travel as JSON values: null, number or string stand for themselves. An INTEGER that a JavaScript
// number cannot hold is {"type":"integer","value":"..."}; a BLOB, or TEXT that is not UTF-8, is
// {"type":"blob","size":N,"value":"<hex of the first bytes>"}.
//
// execute() can answer in binary instead: {"format": "bufs"} returns the rows as a stream of libbufs
// messages (sql/wire/resultset.bufs, written by sql::BufsWriter) in Reply::binary, Content-Type
// application/vnd.libsql.bufs. Everything that is not row data (inTransaction, the time the statement took)
// travels in Reply::headers (X-Sql-In-Transaction, X-Sql-Elapsed-Ms); changes, lastInsertId, truncated and
// the total row count are in the last message. Errors stay JSON whatever the format.
//
// Changes the front end makes are not committed one by one. The first one opens a transaction and it stays
// open until commit() or revert(), which is how DB Browser for SQLite behaves. Every reply carries
// "inTransaction" so the front end knows whether changes are pending.
//
// A WebApi is not thread safe: it owns one Database handle and runs one request at a time.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "json/json.hpp"
#include "sql/sql.hpp"
#if SQL_WITH_BUFS
#include "sql/bufs.hpp"
#endif

namespace sqlweb {

struct Reply
{
    int status = 200;
    json::Json body;
    /// Set instead of `body` for a binary answer.
    std::vector<std::uint8_t> binary;
    std::string contentType;
    /// Extra response headers (binary answers carry what the JSON body would).
    std::vector<std::pair<std::string, std::string>> headers;
};

class WebApi
{
public:
    /// `readOnly` only tells the front end to disable editing; the database enforces it.
    WebApi(sql::Database& db, bool readOnly);

    Reply info();
    Reply schema();
    /// {"sql": "..."}: a script that rebuilds the schema and the rows.
    Reply dump();

    /// {"sql": "...", "maxRows": n, "format": "json" | "bufs"}: runs the statements and returns the rows of
    /// the last one (as JSON unless the format asks for libbufs).
    Reply execute(const json::Json& request);
    /// {"table", "offset", "limit", "sort": [{"column", "descending"}], "filters": {"column": "text"}}
    Reply browse(const json::Json& request);
    /// {"table", "values": {"column": cell}}
    Reply insertRow(const json::Json& request);
    /// {"table", "key": [cell], "set": {"column": cell}}; `key` is what browse() returned for the row.
    Reply updateRow(const json::Json& request);
    /// {"table", "keys": [[cell]]}
    Reply deleteRows(const json::Json& request);

    Reply commit();
    Reply revert();

    Reply pragmas();
    /// {"name", "value"}
    Reply setPragma(const json::Json& request);

private:
    sql::Database& db_;
    bool readOnly_;
#if SQL_WITH_BUFS
    sql::BufsWriter bufs_;  // one request runs at a time, so one writer is enough
#endif
};

}  // namespace sqlweb
