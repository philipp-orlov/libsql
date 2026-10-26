// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What both ends of the remote protocol share: building Reply and Request messages into pooled blocks and
// turning wire parameters and errors into libsql values (sql-remote.md, sections 4, 6.4, 7.2).
#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "http/buffer.hpp"
#include "sql/sql.hpp"
#include "sql/wire/remote.bufs.hpp"

namespace sql::remote::detail {

/// Every field a Reply can carry; unset ones keep their zero defaults and take no space.
struct ReplyFields
{
    std::uint32_t id = 0;
    Kind kind = Kind::None;
    std::uint16_t code = 0;
    std::uint8_t constraint = 0;
    std::string_view message;
    std::uint64_t changes = 0;
    std::int64_t lastInsertId = 0;
    std::int64_t totalChanges = 0;
    bool inTransaction = false;
    std::uint64_t elapsedUs = 0;
    std::uint32_t parameters = 0;
    const std::vector<std::string>* parameterNames = nullptr;
    const std::vector<std::string>* columns = nullptr;
    bool writes = false;
    std::uint16_t protocol = 0;
    std::string_view server;
    std::uint64_t session = 0;
    bool readOnly = false;
    std::uint64_t window = 0;
    std::uint32_t maxMessage = 0;
    std::uint16_t maxPipeline = 0;
    std::string_view sql;
    bool strict = false;
    bool withoutRowid = false;
    std::int64_t handleChanges = 0;
    std::int64_t handleLastInsertId = 0;
};

/// An upper bound of the bytes a Reply with these fields takes.
std::size_t replyBound(const ReplyFields& fields);

/// Builds the Reply into `memory` (8-aligned, `capacity` >= replyBound) and returns its size, a multiple of 8.
std::size_t buildReply(void* memory, std::size_t capacity, const ReplyFields& fields);

/// A pooled block holding the Reply followed by `trailer` (a sql.wire.ResultSet message, say).
http::Buffer makeReply(http::BufferPool& pool, const ReplyFields& fields, std::string_view trailer = {});

/// Window accounting: a Rows frame counts at least this much against the flow-control window, so many
/// tiny frames cannot overrun the connection's queue of frames.
constexpr std::size_t kMinFrameCost = 8192;
inline std::size_t frameCost(std::size_t frameBytes) noexcept
{
    return frameBytes < kMinFrameCost ? kMinFrameCost : frameBytes;
}

constexpr std::uint16_t kProtocolVersion = 1;

/// The error a request ended with, as Reply fields (message must outlive the use).
void errorFields(ReplyFields& fields, const std::exception& e, std::string& scratch);

/// ErrorCode <-> wire value.
inline std::uint16_t wireCode(ErrorCode c) noexcept { return static_cast<std::uint16_t>(c); }
inline ErrorCode errorCodeFrom(std::uint16_t v) noexcept
{
    return v <= static_cast<std::uint16_t>(ErrorCode::PermissionDenied) ? static_cast<ErrorCode>(v) : ErrorCode::Internal;
}

}  // namespace sql::remote::detail
