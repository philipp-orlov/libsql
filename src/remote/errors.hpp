// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// How a sql::Error crosses the wire: as an rpc::Status whose Code is the nearest gRPC-style code and whose detail
// holds the exact ErrorCode and Constraint, so the client throws the same error the server raised.
#pragma once

#include <cstdint>
#include <exception>
#include <string>

#include "nosql/nosql.hpp"
#include "rpc/status.hpp"
#include "sql/error.hpp"

namespace sql::remote::detail {

constexpr std::uint32_t kDetailFlag = 0x80000000u;

inline std::uint32_t errorDetail(ErrorCode code, Constraint constraint) noexcept
{
    return kDetailFlag | (static_cast<std::uint32_t>(code) << 8) | static_cast<std::uint32_t>(constraint);
}

inline rpc::Code rpcCodeFor(ErrorCode code) noexcept
{
    switch (code) {
        case ErrorCode::Interrupted: return rpc::Code::Cancelled;
        case ErrorCode::PermissionDenied:
        case ErrorCode::ReadOnly: return rpc::Code::PermissionDenied;
        case ErrorCode::Busy:
        case ErrorCode::Connection: return rpc::Code::Unavailable;
        case ErrorCode::SyntaxError:
        case ErrorCode::InvalidArgument: return rpc::Code::InvalidArgument;
        case ErrorCode::NoSuchTable:
        case ErrorCode::NoSuchColumn:
        case ErrorCode::NoSuchIndex: return rpc::Code::NotFound;
        case ErrorCode::TableExists:
        case ErrorCode::IndexExists: return rpc::Code::AlreadyExists;
        case ErrorCode::Unsupported: return rpc::Code::Unimplemented;
        case ErrorCode::Internal: return rpc::Code::Internal;
        case ErrorCode::Protocol: return rpc::Code::DataLoss;
        default: return rpc::Code::Aborted;
    }
}

/// The rpc::Error for an exception a statement raised.
inline rpc::Error toRpcError(const std::exception& e)
{
    if (const auto* error = dynamic_cast<const Error*>(&e))
        return rpc::Error(rpcCodeFor(error->code()), error->what(), errorDetail(error->code(), error->constraint()));
    if (const auto* nosqlError = dynamic_cast<const nosql::Error*>(&e)) {
        const ErrorCode code = nosqlError->code() == nosql::ErrorCode::Busy ? ErrorCode::Busy : ErrorCode::Internal;
        return rpc::Error(rpcCodeFor(code), e.what(), errorDetail(code, Constraint::None));
    }
    return rpc::Error(rpc::Code::Internal, e.what(), errorDetail(ErrorCode::Internal, Constraint::None));
}

/// The sql::Error a status stands for (client side).
inline Error fromRpcStatus(const rpc::Status& status)
{
    if (status.detail() & kDetailFlag) {
        const std::uint32_t d = status.detail();
        const auto code = static_cast<ErrorCode>((d >> 8) & 0xFFFF);
        const auto constraint = static_cast<Constraint>(d & 0xFF);
        return Error(code, constraint, status.message());
    }
    switch (status.code()) {
        case rpc::Code::Cancelled: return Error(ErrorCode::Interrupted, status.message().empty() ? "interrupted" : status.message());
        case rpc::Code::DeadlineExceeded: return Error(ErrorCode::Interrupted, "interrupted: the request timed out");
        case rpc::Code::Unauthenticated:
        case rpc::Code::PermissionDenied: return Error(ErrorCode::PermissionDenied, status.message());
        case rpc::Code::Unavailable: return Error(ErrorCode::Connection, status.message());
        case rpc::Code::InvalidArgument: return Error(ErrorCode::InvalidArgument, status.message());
        case rpc::Code::ResourceExhausted: return Error(ErrorCode::Busy, status.message());
        case rpc::Code::DataLoss: return Error(ErrorCode::Protocol, status.message());
        default: return Error(ErrorCode::Internal, status.toString());
    }
}

}  // namespace sql::remote::detail
