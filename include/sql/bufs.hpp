// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libsql -- query results as binary, column-oriented libbufs messages, ready to
// put on the wire (schema: sql/wire/resultset.bufs).
//
//     sql::BufsWriter writer;     // one per thread / worker, reused
//     auto bytes = writer.exec(db, "SELECT id, name, price FROM orders WHERE price < ?",
//                              {sql::Value(20.0)});
//     send(bytes.data(), bytes.size());
//
// The SQL is plain SQL: the format is chosen by calling this API, not by a
// clause in the query. Rows go straight from the executor into per-column
// staging buffers (no sql::Result in between), columns are narrowed (small
// integer deltas, float32 reals, whole-second datetimes, dictionary-coded
// text) and the message is packed in one go. A warm writer allocates nothing.
#pragma once

#if !SQL_WITH_BUFS
#error "libsql was built without SQL_WITH_BUFS"
#endif

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "sql/sql.hpp"

namespace sql {

/// Encoding options.
class BufsOptions
{
public:
    /// Rows per message; 0 = one message for the whole result (the default).
    BufsOptions& batchRows(std::size_t n) noexcept
    {
        batchRows_ = n;
        return *this;
    }
    /// Close a batch once its staged data reaches about n bytes (0 = no byte
    /// limit, the default), whichever of batchRows and batchBytes comes first.
    /// A batch never exceeds n by more than one row. For streaming over a
    /// transport with a message size limit.
    BufsOptions& batchBytes(std::size_t n) noexcept
    {
        batchBytes_ = n;
        return *this;
    }
    /// Keep at most n rows (0 = all, the default). The rest are not encoded and
    /// the result is marked truncated.
    BufsOptions& maxRows(std::size_t n) noexcept
    {
        maxRows_ = n;
        return *this;
    }
    /// With maxRows: keep reading (without storing) to report the full row
    /// count. Default false: the statement stops at row maxRows + 1, and
    /// total_rows is then only known to exceed maxRows.
    BufsOptions& countAllRows(bool on = true) noexcept
    {
        countAllRows_ = on;
        return *this;
    }
    /// Store only the first n bytes of each BLOB, plus its full size
    /// (0 = whole blobs, the default).
    BufsOptions& blobPrefix(std::size_t n) noexcept
    {
        blobPrefix_ = n;
        return *this;
    }
    /// Dictionary-code repeated text (default on).
    BufsOptions& dictionary(bool on) noexcept
    {
        dictionary_ = on;
        return *this;
    }
    /// Narrow integers, reals and datetimes (default on); off stores raw
    /// i64/f64, for debugging and benchmarks.
    BufsOptions& narrowing(bool on) noexcept
    {
        narrowing_ = on;
        return *this;
    }

    std::size_t batchRows() const noexcept { return batchRows_; }
    std::size_t batchBytes() const noexcept { return batchBytes_; }
    std::size_t maxRows() const noexcept { return maxRows_; }
    /// (Named apart from the setter, whose `on` defaults to true.)
    bool countsAllRows() const noexcept { return countAllRows_; }
    std::size_t blobPrefix() const noexcept { return blobPrefix_; }
    bool dictionary() const noexcept { return dictionary_; }
    bool narrowing() const noexcept { return narrowing_; }

private:
    std::size_t batchRows_ = 0;
    std::size_t batchBytes_ = 0;
    std::size_t maxRows_ = 0;
    std::size_t blobPrefix_ = 0;
    bool countAllRows_ = false;
    bool dictionary_ = true;
    bool narrowing_ = true;
};

/// The bytes of one or more sql.wire.ResultSet messages, 8-aligned, in the
/// writer's buffer. Valid until the next call on the writer.
class BufsBytes
{
public:
    constexpr BufsBytes() noexcept = default;
    constexpr BufsBytes(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}
    constexpr const std::uint8_t* data() const noexcept { return data_; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr const std::uint8_t* begin() const noexcept { return data_; }
    constexpr const std::uint8_t* end() const noexcept { return data_ + size_; }
    constexpr std::uint8_t operator[](std::size_t i) const noexcept { return data_[i]; }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

/// A reusable encoder of query results into libbufs messages. Keep one per
/// thread (or per worker) and reuse it: a warm writer allocates nothing.
/// Move-only, and not safe to use from two threads at once.
class BufsWriter final : public ResultConsumer
{
public:
    BufsWriter();
    explicit BufsWriter(BufsOptions options);
    ~BufsWriter() override;
    BufsWriter(BufsWriter&&) noexcept;
    BufsWriter& operator=(BufsWriter&&) noexcept;

    const BufsOptions& options() const noexcept;
    BufsWriter& options(BufsOptions options);

    /// Run and encode in one step. Same semantics as Database::exec etc.: the
    /// bytes encode the rows of the last statement, or one message with no
    /// columns (and the change counters) when it returns none.
    BufsBytes exec(Database& db, std::string_view text, const Params& params = {});
    BufsBytes exec(Database& db, std::string_view text, const NamedParams& params);
    BufsBytes exec(Transaction& txn, std::string_view text, const Params& params = {});
    BufsBytes exec(Statement& stmt);
    BufsBytes exec(Statement& stmt, Transaction& txn);

    /// Forgets the previous result (exec() and write() do this themselves); for
    /// code that feeds the writer as a ResultConsumer or by row().
    BufsWriter& reset();

    /// Encode a Result the caller already has.
    BufsBytes write(const Result& result);

    /// Instead of collecting all batches in the buffer, hand each finished
    /// batch over as soon as it is built (exec() then returns nothing). For
    /// streaming responses: memory is bounded by one batch. Pass an empty
    /// function to go back.
    BufsWriter& onBatch(std::function<void(BufsBytes)> sink);

    /// What the last exec()/write() produced.
    BufsBytes bytes() const noexcept;
    /// Bytes held in buffers and staging.
    std::size_t capacity() const noexcept;
    /// Give that memory back.
    void compact();

    // ResultConsumer: usable directly with Database::exec(text, params, writer).
    void begin(const ResultShape& shape) override;
    bool row(const RowView& row) override;
    /// The same for values the caller holds (rows built by hand, or read from
    /// elsewhere): `count` cells, one per column. False once the writer wants
    /// no more rows (maxRows reached).
    bool row(const Value* values, std::size_t count);
    void end(std::uint64_t changes, std::int64_t lastInsertId) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// One-shot, for code that is not in a loop.
std::vector<std::uint8_t> execBufs(Database& db, std::string_view text, const Params& params = {},
                                   const BufsOptions& options = BufsOptions());

}  // namespace sql
