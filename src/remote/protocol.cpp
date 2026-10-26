// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "remote/protocol.hpp"

#include <cstring>

#include "bufs/bufs.hpp"
#include "nosql/nosql.hpp"

namespace sql::remote::detail {

std::size_t replyBound(const ReplyFields& f)
{
    std::size_t bytes = bufs::kHeaderBytes + bufs::tableBytes(Reply::kDataBytes, Reply::kRefCount) + 16;
    bytes += bufs::stringBytes(f.message.size()) + bufs::stringBytes(f.server.size()) + bufs::stringBytes(f.sql.size());
    if (f.parameterNames) {
        bytes += bufs::vectorBytes(f.parameterNames->size(), 4, 4);
        for (const std::string& s : *f.parameterNames)
            bytes += bufs::stringBytes(s.size());
    }
    if (f.columns) {
        bytes += bufs::vectorBytes(f.columns->size(), 4, 4);
        for (const std::string& s : *f.columns)
            bytes += bufs::stringBytes(s.size());
    }
    return bytes;
}

std::size_t buildReply(void* memory, std::size_t capacity, const ReplyFields& f)
{
    bufs::MessageBuilder mb(memory, capacity);
    auto r = mb.initRoot<Reply>();
    r.setId(f.id);
    r.setKind(f.kind);
    r.setCode(f.code);
    r.setConstraint(f.constraint);
    r.setChanges(f.changes);
    r.setLastInsertId(f.lastInsertId);
    r.setTotalChanges(f.totalChanges);
    r.setInTransaction(f.inTransaction);
    r.setElapsedUs(f.elapsedUs);
    r.setParameters(f.parameters);
    r.setWrites(f.writes);
    r.setProtocol(f.protocol);
    r.setSession(f.session);
    r.setReadOnly(f.readOnly);
    r.setWindow(f.window);
    r.setMaxMessage(f.maxMessage);
    r.setMaxPipeline(f.maxPipeline);
    r.setStrict(f.strict);
    r.setWithoutRowid(f.withoutRowid);
    if (!f.message.empty())
        r.setMessage(f.message);
    if (!f.server.empty())
        r.setServer(f.server);
    if (!f.sql.empty())
        r.setSql(f.sql);
    if (f.parameterNames && !f.parameterNames->empty()) {
        auto list = r.initParameterNames(f.parameterNames->size());
        for (std::size_t i = 0; i < f.parameterNames->size(); ++i)
            list.set(i, (*f.parameterNames)[i]);
    }
    if (f.columns && !f.columns->empty()) {
        auto list = r.initColumns(f.columns->size());
        for (std::size_t i = 0; i < f.columns->size(); ++i)
            list.set(i, (*f.columns)[i]);
    }
    return mb.finish().second;
}

http::Buffer makeReply(http::BufferPool& pool, const ReplyFields& f, std::string_view trailer)
{
    const std::size_t bound = replyBound(f);
    http::Buffer out(pool, bound + trailer.size());
    const std::size_t head = buildReply(out.data(), bound, f);
    if (!trailer.empty())
        std::memcpy(out.data() + head, trailer.data(), trailer.size());
    out.setSize(head + trailer.size());
    return out;
}

void errorFields(ReplyFields& f, const std::exception& e, std::string& scratch)
{
    f.kind = Kind::Error;
    if (const auto* sqlError = dynamic_cast<const Error*>(&e)) {
        f.code = wireCode(sqlError->code());
        f.constraint = static_cast<std::uint8_t>(sqlError->constraint());
        f.message = sqlError->what();
        return;
    }
    if (const auto* nosqlError = dynamic_cast<const nosql::Error*>(&e)) {
        f.code = wireCode(nosqlError->code() == nosql::ErrorCode::Busy ? ErrorCode::Busy : ErrorCode::Internal);
        f.message = nosqlError->what();
        return;
    }
    f.code = wireCode(ErrorCode::Internal);
    scratch = e.what();
    f.message = scratch;
}

}  // namespace sql::remote::detail
