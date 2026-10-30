// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// SQL_WITH_REMOTE=OFF: Database::connect and everything behind it say so.

#include "remote/client.hpp"

namespace sql::internal {

namespace {
[[noreturn]] void unsupported()
{
    throw Error(ErrorCode::Unsupported, "libsql was built without SQL_WITH_REMOTE");
}
}  // namespace

RemoteTxn::~RemoteTxn() = default;
RemoteStatement::~RemoteStatement() = default;

std::shared_ptr<RemoteSession> remoteConnect(std::string_view, const RemoteOptions&) { unsupported(); }
const std::filesystem::path& remotePath(const RemoteSession&) { unsupported(); }
std::shared_ptr<RemoteSession> remoteSibling(RemoteSession&, bool) { unsupported(); }
Result remoteExec(RemoteSession&, std::string_view, const Params*, const NamedParams*, RemoteTxn*, ResultConsumer*, Result*)
{
    unsupported();
}
std::shared_ptr<RemoteStatement> remotePrepare(const std::shared_ptr<RemoteSession>&, std::string_view) { unsupported(); }
std::shared_ptr<RemoteTxn> remoteBegin(const std::shared_ptr<RemoteSession>&, bool) { unsupported(); }
std::int64_t remoteChanges(const RemoteSession&) { unsupported(); }
std::int64_t remoteTotalChanges(const RemoteSession&) { unsupported(); }
std::int64_t remoteLastInsertRowid(const RemoteSession&) { unsupported(); }
bool remoteInTransaction(const RemoteSession&) { unsupported(); }
void remoteInterrupt(RemoteSession&) noexcept {}
std::vector<std::string> remoteTables(RemoteSession&) { unsupported(); }
TableInfo remoteTable(RemoteSession&, std::string_view) { unsupported(); }
std::vector<IndexInfo> remoteIndexes(RemoteSession&, std::string_view) { unsupported(); }
void remoteClose(RemoteSession&) noexcept {}
void remoteFinishTxn(RemoteTxn&, bool) { unsupported(); }
void remoteAbandonTxn(RemoteTxn&) noexcept {}
Result remoteExecStatement(RemoteStatement&, RemoteTxn*, ResultConsumer*, Result*) { unsupported(); }
void remoteCloseStatement(RemoteStatement&) noexcept {}

}  // namespace sql::internal
