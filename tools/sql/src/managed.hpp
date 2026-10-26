// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The "Execute SQL" tab's transaction behaviour, shared by the JSON API and the WebSocket route.
//
// Like the Execute SQL tab of DB Browser, whatever the statements change stays pending until Write Changes
// (commit) or Revert; a run that changed nothing leaves nothing pending. Statements that manage a transaction
// themselves (or cannot run inside one) are left alone.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>

#include "sql/remote/sql_socket.hpp"
#include "sql/sql.hpp"

namespace sqlweb {

/// True when the statement manages the transaction itself, or cannot run inside one.
inline bool controlsTransaction(std::string_view text)
{
    const auto equalsNoCase = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    };
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
    for (const char* keyword : {"BEGIN", "COMMIT", "END", "ROLLBACK", "SAVEPOINT", "RELEASE", "PRAGMA", "VACUUM",
                                "ATTACH", "DETACH"}) {
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

inline std::int64_t schemaVersion(sql::Database& db)
{
    return db.exec("PRAGMA schema_version")[0][0].toInteger();
}

/// What the Execute SQL tab does around a statement: open a transaction first, and roll it back afterwards when
/// the run failed or changed nothing. Runs on the one worker that owns the Database, so its per-run memory can
/// be plain members.
class ManagedExec final : public sql::remote::ExecInterceptor
{
public:
    /// Request.hints bit 0: this request wants the behaviour (the Execute SQL tab sets it).
    static constexpr std::uint32_t kManaged = 1;

    void before(sql::Database& db, std::string_view text, std::uint32_t hints) override
    {
        began_ = false;
        if (!(hints & kManaged) || db.inTransaction() || controlsTransaction(text))
            return;
        changesBefore_ = db.totalChanges();
        schemaBefore_ = schemaVersion(db);
        db.exec("BEGIN");
        began_ = true;
    }

    void after(sql::Database& db, std::string_view, std::uint32_t, bool failed) noexcept override
    {
        if (!began_)
            return;
        began_ = false;
        try {
            if (db.inTransaction() && (failed || (db.totalChanges() == changesBefore_ && schemaVersion(db) == schemaBefore_)))
                db.exec("ROLLBACK");
        } catch (...) {
        }
    }

private:
    bool began_ = false;
    std::int64_t changesBefore_ = 0;
    std::int64_t schemaBefore_ = 0;
};

}  // namespace sqlweb
