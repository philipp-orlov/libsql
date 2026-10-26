// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "remote/protocol.hpp"

#include <cstring>

#include "bufs/bufs.hpp"
#include "nosql/nosql.hpp"

namespace sql::remote::detail {

namespace {

void encodeValue(Param::Builder p, const Value& v)
{
    switch (v.type()) {
        case Type::Integer:
            p.setType(ParamType::Integer);
            p.setInteger(v.integer());
            break;
        case Type::Real:
            p.setType(ParamType::Real);
            p.setReal(v.real());
            break;
        case Type::Datetime:
            p.setType(ParamType::Datetime);
            p.setInteger(v.datetime().usec);
            break;
        case Type::Text: {
            p.setType(ParamType::Text);
            const std::string_view t = v.text();
            p.setBytes(reinterpret_cast<const std::uint8_t*>(t.data()), t.size());
            break;
        }
        case Type::Blob: {
            p.setType(ParamType::Blob);
            const BlobView b = v.blob();
            p.setBytes(reinterpret_cast<const std::uint8_t*>(b.data()), b.size());
            break;
        }
        default: break;
    }
}

}  // namespace

std::pair<const std::uint8_t*, std::size_t> buildRequest(bufs::MessageBuilder& builder, const RequestFields& f)
{
    auto q = builder.initRoot<Request>();
    q.setId(f.id);
    q.setOp(f.op);
    q.setStatement(f.statement);
    q.setTxn(f.txn);
    q.setTarget(f.target);
    q.setCredit(f.credit);
    q.setReadOnly(f.readOnly);
    q.setWhat(f.what);
    q.setProtocol(f.protocol);
    q.setWindow(f.window);
    q.setMaxMessage(f.maxMessage);
    q.setHints(f.hints);
    if (!f.sql.empty())
        q.setSql(f.sql);
    if (f.named) {
        if (!f.named->empty()) {
            auto list = q.initParams(f.named->size());
            std::size_t i = 0;
            for (const auto& [name, v] : *f.named) {
                auto p = list.init(i++);
                p.setName(name);
                encodeValue(p, v);
            }
        }
    } else if (f.paramCount) {
        auto list = q.initParams(f.paramCount);
        for (std::size_t i = 0; i < f.paramCount; ++i)
            encodeValue(list.init(i), f.params[i]);
    }
    if (f.options) {
        auto o = q.initOptions();
        o.setMaxRows(f.maxRows);
        o.setBatchRows(f.batchRows);
        o.setBatchBytes(f.batchBytes);
        o.setBlobPrefix(f.blobPrefix);
        o.setCountAllRows(f.countAllRows);
        o.setNoDictionary(f.noDictionary);
        o.setNoNarrowing(f.noNarrowing);
    }
    if (!f.name.empty())
        q.setName(f.name);
    if (!f.client.empty())
        q.setClient(f.client);
    if (!f.token.empty())
        q.setToken(f.token);
    return builder.finish();
}

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
    r.setHandleChanges(f.handleChanges);
    r.setHandleLastInsertId(f.handleLastInsertId);
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
    // The message is copied: the caller builds the reply after the exception object is gone.
    scratch = e.what();
    f.message = scratch;
    if (const auto* sqlError = dynamic_cast<const Error*>(&e)) {
        f.code = wireCode(sqlError->code());
        f.constraint = static_cast<std::uint8_t>(sqlError->constraint());
        return;
    }
    if (const auto* nosqlError = dynamic_cast<const nosql::Error*>(&e)) {
        f.code = wireCode(nosqlError->code() == nosql::ErrorCode::Busy ? ErrorCode::Busy : ErrorCode::Internal);
        return;
    }
    f.code = wireCode(ErrorCode::Internal);
}

}  // namespace sql::remote::detail
