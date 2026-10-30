// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The client half of remote databases (sql-remote.md, section 7): what Database, Transaction and Statement call
// when they were made by Database::connect. RemoteSession is opaque here; the functions below are defined in
// client.cpp (SQL_WITH_REMOTE) or fail with Unsupported in client_stub.cpp.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sql/sql.hpp"

namespace sql::internal {

/// What the server said about a prepared statement.
struct RemoteStatementInfo
{
    std::string sql;
    std::size_t parameters = 0;
    std::vector<std::string> names;    ///< spelling of each parameter, "" for a plain `?`
    std::vector<std::string> columns;
    bool writes = false;
};

struct RemoteTxn
{
    std::shared_ptr<RemoteSession> session;
    std::uint32_t id = 0;
    std::uint64_t epoch = 0;  ///< the connection it lives on; a new connection makes it stale
    bool open = true;
    ~RemoteTxn();
};

struct RemoteStatement
{
    std::shared_ptr<RemoteSession> session;
    std::uint32_t id = 0;
    std::uint64_t epoch = 0;  ///< the connection it was prepared on
    RemoteStatementInfo info;
    std::vector<Value> bound;
    ~RemoteStatement();
};

// --- Database ---
std::shared_ptr<RemoteSession> remoteConnect(std::string_view url, const RemoteOptions& options);
const std::filesystem::path& remotePath(const RemoteSession& session);
std::shared_ptr<RemoteSession> remoteSibling(RemoteSession& session, bool readOnly);
/// Runs text on the server. Rows go to `consumer` when given, else into `into` when given, else into the
/// returned Result. `txn` runs it inside that transaction. `named` replaces `params` when set.
Result remoteExec(RemoteSession& session, std::string_view text, const Params* params, const NamedParams* named,
                  RemoteTxn* txn, ResultConsumer* consumer, Result* into);
std::shared_ptr<RemoteStatement> remotePrepare(const std::shared_ptr<RemoteSession>& session, std::string_view text);
std::shared_ptr<RemoteTxn> remoteBegin(const std::shared_ptr<RemoteSession>& session, bool readOnly);
std::int64_t remoteChanges(const RemoteSession& session);
std::int64_t remoteTotalChanges(const RemoteSession& session);
std::int64_t remoteLastInsertRowid(const RemoteSession& session);
bool remoteInTransaction(const RemoteSession& session);
void remoteInterrupt(RemoteSession& session) noexcept;
std::vector<std::string> remoteTables(RemoteSession& session);
TableInfo remoteTable(RemoteSession& session, std::string_view name);
std::vector<IndexInfo> remoteIndexes(RemoteSession& session, std::string_view table);
void remoteClose(RemoteSession& session) noexcept;

// --- Transaction ---
void remoteFinishTxn(RemoteTxn& txn, bool commit);
void remoteAbandonTxn(RemoteTxn& txn) noexcept;

// --- Statement ---
Result remoteExecStatement(RemoteStatement& statement, RemoteTxn* txn, ResultConsumer* consumer, Result* into);
void remoteCloseStatement(RemoteStatement& statement) noexcept;

}  // namespace sql::internal
