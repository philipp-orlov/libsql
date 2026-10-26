// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libsql -- reading what sql::BufsWriter wrote (sql/wire/resultset.bufs), on top
// of the bufsc-generated readers. Section 3.7 of the format: how one cell is
// found. Header-only except toResult().
//
//     auto view = sql::wire::ResultView::open(bytes.data(), bytes.size());
//     for (std::uint64_t r = 0; r < view.rowCount(); ++r)
//         use(view.value(r, 0));
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bufs/bufs.hpp"
#include "sql/sql.hpp"
#include "sql/wire/resultset.bufs.hpp"

namespace sql::wire {

/// One column of one batch. Cheap to copy; valid while the ResultView it came
/// from lives.
class ColumnView
{
public:
    ColumnView(Column::Reader column, std::size_t rows, std::vector<std::uint32_t>* slots) noexcept
        : c_(column), rows_(rows), slots_(slots), encoding_(column.encoding())
    {}

    Encoding encoding() const { return encoding_; }
    /// Rows in the batch.
    std::size_t size() const { return rows_; }
    Width width() const { return c_.width(); }
    std::uint32_t nullCount() const { return c_.nullCount(); }

    bool isNull(std::size_t row) const
    {
        if (encoding_ == Encoding::Empty)
            return true;
        if (encoding_ == Encoding::Mixed)
            return c_.tags()[row] == std::uint8_t(Tag::Null);
        return c_.nullCount() > 0 && ((c_.nulls()[row >> 3] >> (row & 7)) & 1);
    }

    /// The storage class of that cell.
    sql::Type type(std::size_t row) const
    {
        if (isNull(row))
            return sql::Type::Null;
        switch (encoding_) {
            case Encoding::Int: return sql::Type::Integer;
            case Encoding::Real: return sql::Type::Real;
            case Encoding::Datetime: return sql::Type::Datetime;
            case Encoding::Text:
            case Encoding::TextDict: return sql::Type::Text;
            case Encoding::Blob: return sql::Type::Blob;
            case Encoding::Mixed: return sql::Type(c_.tags()[row]);
            default: return sql::Type::Null;
        }
    }

    std::int64_t integer(std::size_t row) const
    {
        if (encoding_ == Encoding::Mixed)
            return c_.i64()[slot(row)];
        return std::int64_t(std::uint64_t(c_.base()) + delta(row));
    }

    double real(std::size_t row) const
    {
        if (encoding_ == Encoding::Mixed)
            return c_.f64()[slot(row)];
        return c_.hasF32() ? double(c_.f32()[row]) : c_.f64()[row];
    }

    sql::Datetime datetime(std::size_t row) const
    {
        if (encoding_ == Encoding::Mixed)
            return sql::Datetime{c_.i64()[slot(row)]};
        if (c_.width() == Width::I64)
            return sql::Datetime{c_.i64()[row]};
        return sql::Datetime{std::int64_t(std::uint64_t(c_.base()) + delta(row) * unit())};
    }

    std::string_view text(std::size_t row) const
    {
        if (encoding_ == Encoding::TextDict) {
            const std::size_t code = c_.width() == Width::U8 ? c_.u8()[row] : c_.u16()[row];
            const auto offsets = c_.dictOffsets();
            return chars(c_.dictBytes().data(), offsets[code], offsets[code + 1]);
        }
        const std::size_t i = encoding_ == Encoding::Mixed ? slot(row) : row;
        const auto offsets = c_.offsets();
        return chars(c_.bytes().data(), offsets[i], offsets[i + 1]);
    }

    /// The stored bytes of a BLOB: the whole blob, or the prefix the writer kept.
    sql::BlobView blob(std::size_t row) const
    {
        const std::string_view b = text(row);
        return sql::BlobView(reinterpret_cast<const std::byte*>(b.data()), b.size());
    }

    /// The full size of a BLOB (or the length of a text).
    std::uint64_t blobSize(std::size_t row) const
    {
        const auto sizes = c_.blobSizes();
        if (sizes.empty())
            return text(row).size();
        return sizes[encoding_ == Encoding::Mixed ? slot(row) : row];
    }

    /// A borrowed Value (textRef / blobRef): copy it to keep it.
    sql::Value value(std::size_t row) const
    {
        switch (type(row)) {
            case sql::Type::Integer: return sql::Value(integer(row));
            case sql::Type::Real: return sql::Value(real(row));
            case sql::Type::Datetime: return sql::Value(datetime(row));
            case sql::Type::Text: return sql::Value::textRef(text(row));
            case sql::Type::Blob: {
                const sql::BlobView b = blob(row);
                return sql::Value::blobRef(b.data(), b.size());
            }
            default: return sql::Value();
        }
    }

    // Zero-copy typed access, for code that scans a column.
    const std::uint8_t* deltas8() const { return c_.u8().data(); }
    const std::uint16_t* deltas16() const { return c_.u16().data(); }
    const std::uint32_t* deltas32() const { return c_.u32().data(); }
    const std::int64_t* int64s() const { return c_.i64().data(); }
    const float* f32() const { return c_.f32().data(); }
    const double* f64() const { return c_.f64().data(); }
    std::int64_t base() const { return c_.base(); }
    Scale scale() const { return c_.scale(); }

private:
    std::uint64_t delta(std::size_t row) const
    {
        switch (c_.width()) {
            case Width::U8: return c_.u8()[row];
            case Width::U16: return c_.u16()[row];
            case Width::U32: return c_.u32()[row];
            default: return std::uint64_t(c_.i64()[row]) - std::uint64_t(c_.base());
        }
    }
    std::uint64_t unit() const
    {
        switch (c_.scale()) {
            case Scale::Day: return 86400000000ull;
            case Scale::Second: return 1000000ull;
            case Scale::Milli: return 1000ull;
            default: return 1;
        }
    }
    static std::string_view chars(const std::uint8_t* bytes, std::size_t from, std::size_t to)
    {
        return std::string_view(reinterpret_cast<const char*>(bytes) + from, to - from);
    }
    /// Index of the row in its pool (integers and datetimes, reals, or texts and blobs).
    std::size_t slot(std::size_t row) const
    {
        if (slots_->empty() && rows_) {
            slots_->resize(rows_);
            std::uint32_t ints = 0, reals = 0, bytes = 0;
            const auto tags = c_.tags();
            for (std::size_t i = 0; i < rows_; ++i) {
                switch (Tag(tags[i])) {
                    case Tag::Integer:
                    case Tag::Datetime: (*slots_)[i] = ints++; break;
                    case Tag::Real: (*slots_)[i] = reals++; break;
                    case Tag::Text:
                    case Tag::Blob: (*slots_)[i] = bytes++; break;
                    default: (*slots_)[i] = 0; break;
                }
            }
        }
        return (*slots_)[row];
    }

    Column::Reader c_;
    std::size_t rows_;
    std::vector<std::uint32_t>* slots_;
    Encoding encoding_;
};

/// One message of a stream: a range of rows.
class BatchView
{
public:
    /// Verifies the message first; empty when it is not a ResultSet. `slots`, if given, is where the
    /// per-column index of Mixed columns is built (kept by the caller between batches, so reading a
    /// stream of batches allocates nothing once warm); the caller clears it for each new batch.
    static std::optional<BatchView> open(const std::uint8_t* message, std::size_t size,
                                         std::vector<std::vector<std::uint32_t>>* slots = nullptr)
    {
        if (!::bufs::verify<ResultSet>(message, size))
            return std::nullopt;
        BatchView view(message);
        view.external_ = slots;
        return view;
    }

    std::uint64_t rowOffset() const { return r_.rowOffset(); }
    std::uint32_t rowCount() const { return r_.rowCount(); }
    bool more() const { return r_.more(); }
    std::size_t columnCount() const { return r_.data().size(); }
    ColumnView column(std::size_t i) const
    {
        auto& slots = external_ ? *external_ : slots_;
        if (slots.size() < columnCount())
            slots.resize(columnCount());
        return ColumnView(r_.data()[i], r_.rowCount(), &slots[i]);
    }
    ResultSet::Reader reader() const { return r_; }

private:
    explicit BatchView(const std::uint8_t* message) : r_(::bufs::root<ResultSet>(message)) {}
    ResultSet::Reader r_;
    mutable std::vector<std::vector<std::uint32_t>> slots_;  ///< Mixed slot indexes, built on first use
    std::vector<std::vector<std::uint32_t>>* external_ = nullptr;  ///< the caller's, when it keeps them
};

/// All batches of a stream, with the column descriptions of the first.
class ResultView
{
public:
    /// Verifies every message; copies into an aligned buffer when `data` is not
    /// 8-aligned. Throws sql::Error(InvalidArgument) on anything else.
    static ResultView open(const void* data, std::size_t size)
    {
        ResultView v;
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
        if (size && (reinterpret_cast<std::uintptr_t>(bytes) & 7) != 0) {
            v.owned_.resize((size + 7) / 8);
            std::memcpy(v.owned_.data(), data, size);
            bytes = reinterpret_cast<const std::uint8_t*>(v.owned_.data());
        }
        if (size == 0 || !::bufs::verifyStream<ResultSet>(bytes, size))
            fail();
        ::bufs::MessageStream stream(bytes, size);
        const std::uint8_t* message = nullptr;
        std::size_t messageSize = 0;
        while (stream.next(message, messageSize)) {
            auto batch = BatchView::open(message, messageSize);
            if (!batch)
                fail();
            v.batches_.push_back(std::move(*batch));
        }
        const auto first = v.batches_.front().reader();
        bool anyQualified = false;
        for (auto info : first.columns())
            anyQualified = anyQualified || info.hasQualified();
        for (auto info : first.columns()) {
            v.columns_.emplace_back(info.name());
            if (anyQualified)
                v.qualified_.emplace_back(info.qualified());
            v.declTypes_.emplace_back(info.declType());
        }
        const auto last = v.batches_.back().reader();
        v.truncated_ = last.truncated();
        v.totalRows_ = last.totalRows();
        v.changes_ = last.changes();
        v.lastInsertId_ = last.lastInsertId();
        const auto& tail = v.batches_.back();
        v.rowCount_ = tail.rowOffset() + tail.rowCount();
        return v;
    }

    ResultView(ResultView&&) noexcept = default;
    ResultView& operator=(ResultView&&) noexcept = default;
    ResultView(const ResultView&) = delete;  // the batches point into the buffer
    ResultView& operator=(const ResultView&) = delete;

    const std::vector<std::string>& columns() const { return columns_; }
    const std::vector<std::string>& qualifiedColumns() const { return qualified_; }
    const std::vector<std::string>& declTypes() const { return declTypes_; }
    std::uint64_t rowCount() const { return rowCount_; }
    std::size_t batchCount() const { return batches_.size(); }
    const BatchView& batch(std::size_t i) const { return batches_[i]; }

    /// A borrowed Value; the batch is found by binary search on rowOffset.
    sql::Value value(std::uint64_t row, std::size_t col) const
    {
        const BatchView& b = batchOf(row);
        return b.column(col).value(std::size_t(row - b.rowOffset()));
    }

    bool truncated() const { return truncated_; }
    std::uint64_t totalRows() const { return totalRows_; }
    std::uint64_t changes() const { return changes_; }
    std::int64_t lastInsertId() const { return lastInsertId_; }

private:
    ResultView() = default;
    [[noreturn]] static void fail()
    {
        throw sql::Error(sql::ErrorCode::InvalidArgument, "not a valid libsql bufs result");
    }
    const BatchView& batchOf(std::uint64_t row) const
    {
        std::size_t lo = 0, hi = batches_.size();
        while (hi - lo > 1) {
            const std::size_t mid = (lo + hi) / 2;
            if (batches_[mid].rowOffset() <= row)
                lo = mid;
            else
                hi = mid;
        }
        return batches_[lo];
    }

    std::vector<std::uint64_t> owned_;
    std::vector<BatchView> batches_;
    std::vector<std::string> columns_, qualified_, declTypes_;
    std::uint64_t rowCount_ = 0, totalRows_ = 0, changes_ = 0;
    std::int64_t lastInsertId_ = 0;
    bool truncated_ = false;
};

/// An owning copy as a Result, for tests and tools.
sql::Result toResult(const ResultView& view);

}  // namespace sql::wire
