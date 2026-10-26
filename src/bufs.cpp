// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// sql::BufsWriter: rows are copied into per-column staging while the statement
// runs (a RowView is only valid during the call), the column's encoding is
// chosen from running statistics once the batch is complete, and the message
// is packed in one go into a block sized by an upper bound. See
// docs/design.md and sql/wire/resultset.bufs for the format.

#include "sql/bufs.hpp"

#include "internal/stmt.hpp"
#include "sql/wire/result_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "bufs/bufs.hpp"
#include "sql/wire/resultset.bufs.hpp"

namespace sql {
namespace {


constexpr std::size_t kDictionaryLimit = 65536;
constexpr std::int64_t kUnitMicro[] = {86400000000LL, 1000000LL, 1000LL};  // Day, Second, Milli

/// Bytes that start 8-aligned and keep their content when they grow.
class AlignedBuffer
{
public:
    std::uint8_t* data() noexcept { return reinterpret_cast<std::uint8_t*>(mem_.get()); }
    const std::uint8_t* data() const noexcept { return reinterpret_cast<const std::uint8_t*>(mem_.get()); }
    std::size_t capacity() const noexcept { return capacity_; }
    /// Room for `bytes`, keeping the first `keep`.
    void reserve(std::size_t bytes, std::size_t keep)
    {
        if (bytes <= capacity_)
            return;
        const std::size_t words = (std::max(bytes, capacity_ * 2) + 7) / 8;
        std::unique_ptr<std::uint64_t[]> bigger(new std::uint64_t[words]);
        if (keep)
            std::memcpy(bigger.get(), mem_.get(), keep);
        mem_ = std::move(bigger);
        capacity_ = words * 8;
    }
    void release() noexcept
    {
        mem_.reset();
        capacity_ = 0;
    }

private:
    std::unique_ptr<std::uint64_t[]> mem_;
    std::size_t capacity_ = 0;
};

template <class T>
void releaseVector(std::vector<T>& v)
{
    std::vector<T>().swap(v);
}

/// Distinct texts of a column, keyed by where their first occurrence sits in
/// the column's arena: the arena grows and moves, so nothing points into it.
class Dictionary
{
public:
    std::size_t size() const noexcept { return start_.size(); }
    std::uint32_t start(std::size_t i) const noexcept { return start_[i]; }
    std::uint32_t length(std::size_t i) const noexcept { return length_[i]; }

    /// The code of `text`; a new entry starting at `arenaPos` (where the caller
    /// is about to append it) when unseen. -1 when the table is full.
    int find(const std::uint8_t* arena, std::string_view text, std::uint32_t arenaPos)
    {
        if (slots_.empty())
            slots_.assign(64, 0);
        const std::uint32_t h = hash(text);
        std::size_t mask = slots_.size() - 1;
        for (std::size_t at = h & mask;; at = (at + 1) & mask) {
            const std::uint32_t s = slots_[at];
            if (s == 0)
                break;
            const std::size_t i = s - 1;
            if (length_[i] == text.size() &&
                (text.empty() || std::memcmp(arena + start_[i], text.data(), text.size()) == 0))
                return int(i);
        }
        if (start_.size() >= kDictionaryLimit)
            return -1;
        if ((start_.size() + 1) * 10 > slots_.size() * 7) {
            rehash(arena, slots_.size() * 2);
            mask = slots_.size() - 1;
        }
        std::size_t at = h & mask;
        while (slots_[at] != 0)
            at = (at + 1) & mask;
        start_.push_back(arenaPos);
        length_.push_back(std::uint32_t(text.size()));
        slotOf_.push_back(std::uint32_t(at));
        slots_[at] = std::uint32_t(start_.size());
        return int(start_.size() - 1);
    }

    /// Empties the table, keeping its capacity.
    void clear() noexcept
    {
        for (std::uint32_t at : slotOf_)
            slots_[at] = 0;
        start_.clear();
        length_.clear();
        slotOf_.clear();
    }
    void release()
    {
        releaseVector(slots_);
        releaseVector(start_);
        releaseVector(length_);
        releaseVector(slotOf_);
    }
    std::size_t capacity() const noexcept
    {
        return 4 * (slots_.capacity() + start_.capacity() + length_.capacity() + slotOf_.capacity());
    }

private:
    static std::uint32_t hash(std::string_view text) noexcept
    {
        std::uint32_t h = 2166136261u;
        for (char c : text) {
            h ^= std::uint8_t(c);
            h *= 16777619u;
        }
        return h ^ (h >> 15);
    }
    void rehash(const std::uint8_t* arena, std::size_t capacity)
    {
        slots_.assign(capacity, 0);
        const std::size_t mask = capacity - 1;
        for (std::size_t i = 0; i < start_.size(); ++i) {
            std::size_t at = hash(std::string_view(reinterpret_cast<const char*>(arena + start_[i]), length_[i])) & mask;
            while (slots_[at] != 0)
                at = (at + 1) & mask;
            slots_[at] = std::uint32_t(i + 1);
            slotOf_[i] = std::uint32_t(at);
        }
    }

    std::vector<std::uint32_t> slots_;  ///< entry index + 1, 0 = empty
    std::vector<std::uint32_t> start_, length_, slotOf_;
};

struct ColumnStats
{
    std::uint32_t nulls = 0, ints = 0, reals = 0, datetimes = 0, texts = 0, blobs = 0;
    std::int64_t intLo = std::numeric_limits<std::int64_t>::max();
    std::int64_t intHi = std::numeric_limits<std::int64_t>::min();
    std::int64_t dtLo = std::numeric_limits<std::int64_t>::max();
    std::int64_t dtHi = std::numeric_limits<std::int64_t>::min();
    bool dtFirst = true;
    std::int64_t dtRem[3] = {};
    bool dtDivisible[3] = {true, true, true};
    bool realsFitFloat = true;
    bool anyBlobCut = false;
    std::uint64_t dictTextBytes = 0;
};

std::int64_t remainder(std::int64_t v, std::int64_t unit) noexcept
{
    return ((v % unit) + unit) % unit;
}

/// One column of the batch being collected.
struct ColumnStage
{
    std::vector<std::uint8_t> tags;      ///< sql::Type of each cell
    /// Integer: the value. Real: the bits. Datetime: microseconds.
    /// Text/Blob: the end offset in `arena`. Null: 0.
    std::vector<std::uint64_t> words;
    std::vector<std::uint8_t> arena;     ///< text, and blob bytes up to the prefix limit
    std::vector<std::uint32_t> fullSizes;  ///< full size of each text/blob cell
    std::vector<std::uint16_t> codes;    ///< dictionary code of each row, while the dictionary lives
    Dictionary dict;
    ColumnStats stats;
    bool dictAlive = true;

    void clear(bool dictionary)
    {
        tags.clear();
        words.clear();
        arena.clear();
        fullSizes.clear();
        codes.clear();
        dict.clear();
        stats = ColumnStats();
        dictAlive = dictionary;
    }
    void release()
    {
        releaseVector(tags);
        releaseVector(words);
        releaseVector(arena);
        releaseVector(fullSizes);
        releaseVector(codes);
        dict.release();
    }
    std::size_t capacity() const noexcept
    {
        return tags.capacity() + 8 * words.capacity() + arena.capacity() + 4 * fullSizes.capacity() +
               2 * codes.capacity() + dict.capacity();
    }

    void add(const Value& v, const BufsOptions& options)
    {
        const Type t = v.type();
        tags.push_back(std::uint8_t(t));
        switch (t) {
            case Type::Null:
                words.push_back(0);
                ++stats.nulls;
                break;
            case Type::Integer: {
                const std::int64_t i = v.integer();
                words.push_back(std::uint64_t(i));
                ++stats.ints;
                stats.intLo = std::min(stats.intLo, i);
                stats.intHi = std::max(stats.intHi, i);
                break;
            }
            case Type::Real: {
                const double d = v.real();
                std::uint64_t bits;
                std::memcpy(&bits, &d, 8);
                words.push_back(bits);
                ++stats.reals;
                if (stats.realsFitFloat) {
                    const double back = double(float(d));
                    stats.realsFitFloat = !std::isnan(d) && std::memcmp(&back, &d, 8) == 0;
                }
                break;
            }
            case Type::Datetime: {
                const std::int64_t us = v.datetime().usec;
                words.push_back(std::uint64_t(us));
                ++stats.datetimes;
                stats.dtLo = std::min(stats.dtLo, us);
                stats.dtHi = std::max(stats.dtHi, us);
                for (int u = 0; u < 3; ++u) {
                    const std::int64_t r = remainder(us, kUnitMicro[u]);
                    if (stats.dtFirst)
                        stats.dtRem[u] = r;
                    else if (r != stats.dtRem[u])
                        stats.dtDivisible[u] = false;
                }
                stats.dtFirst = false;
                break;
            }
            case Type::Text: {
                const std::string_view text = v.text();
                append(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), text.size());
                ++stats.texts;
                if (dictAlive) {
                    const std::uint32_t at = std::uint32_t(arena.size() - text.size());
                    const std::size_t before = dict.size();
                    const int code = dict.find(arena.data(), text, at);
                    if (code < 0) {
                        dictAlive = false;
                        codes.clear();
                    } else {
                        if (dict.size() != before)
                            stats.dictTextBytes += text.size();
                        codes.push_back(std::uint16_t(code));
                    }
                }
                return;
            }
            case Type::Blob: {
                const BlobView b = v.blob();
                const std::size_t keep = options.blobPrefix() ? std::min(b.size(), options.blobPrefix()) : b.size();
                append(reinterpret_cast<const std::uint8_t*>(b.data()), keep, b.size());
                if (keep < b.size())
                    stats.anyBlobCut = true;
                ++stats.blobs;
                break;
            }
        }
        if (dictAlive)
            codes.push_back(0);
    }

private:
    void append(const std::uint8_t* bytes, std::size_t keep, std::size_t full)
    {
        if (arena.size() + keep > 0xFFFFFFFFu)
            throw Error(ErrorCode::Runtime, "result exceeds 4 GiB in one message; use BufsOptions::batchRows");
        arena.insert(arena.end(), bytes, bytes + keep);
        words.push_back(arena.size());
        fullSizes.push_back(std::uint32_t(std::min<std::size_t>(full, 0xFFFFFFFFu)));
    }
};

struct ColumnPlan
{
    wire::Encoding encoding = wire::Encoding::Empty;
    wire::Width width = wire::Width::None;
    wire::Scale scale = wire::Scale::Micro;
    std::int64_t base = 0;
    bool f32 = false;
    std::size_t bytes = 0;  ///< upper bound of what this column adds to the message
};

wire::Width widthOf(std::uint64_t range) noexcept
{
    if (range <= 0xFF)
        return wire::Width::U8;
    if (range <= 0xFFFF)
        return wire::Width::U16;
    if (range <= 0xFFFFFFFFu)
        return wire::Width::U32;
    return wire::Width::I64;
}

std::size_t widthBytes(wire::Width w) noexcept
{
    switch (w) {
        case wire::Width::U8: return 1;
        case wire::Width::U16: return 2;
        case wire::Width::U32: return 4;
        default: return 8;
    }
}

std::size_t vectorOf(std::size_t count, std::size_t elementBytes)
{
    return ::bufs::vectorBytes(count, elementBytes, elementBytes);
}

ColumnPlan decide(const ColumnStage& c, std::size_t n, const BufsOptions& options)
{
    const ColumnStats& st = c.stats;
    ColumnPlan p;
    std::size_t bytes = ::bufs::tableBytes(wire::Column::kDataBytes, wire::Column::kRefCount);
    if (st.nulls)
        bytes += vectorOf((n + 7) / 8, 1);
    const int classes = (st.ints > 0) + (st.reals > 0) + (st.datetimes > 0) + (st.texts > 0) + (st.blobs > 0);
    if (classes == 0) {
        p.encoding = wire::Encoding::Empty;
    } else if (classes > 1) {
        p.encoding = wire::Encoding::Mixed;
        bytes += vectorOf(n, 1) + vectorOf(st.ints + st.datetimes, 8) + vectorOf(st.reals, 8) +
                 vectorOf(std::size_t(st.texts) + st.blobs + 1, 4) + vectorOf(c.arena.size(), 1);
        if (st.anyBlobCut)
            bytes += vectorOf(std::size_t(st.texts) + st.blobs, 4);
    } else if (st.ints) {
        p.encoding = wire::Encoding::Int;
        p.width = wire::Width::I64;
        if (options.narrowing()) {
            p.width = widthOf(std::uint64_t(st.intHi) - std::uint64_t(st.intLo));
            if (p.width != wire::Width::I64)
                p.base = st.intLo;
        }
        bytes += vectorOf(n, widthBytes(p.width));
    } else if (st.datetimes) {
        p.encoding = wire::Encoding::Datetime;
        p.width = wire::Width::I64;
        if (options.narrowing()) {
            int unit = -1;
            for (int u = 0; u < 3 && unit < 0; ++u)
                if (st.dtDivisible[u])
                    unit = u;
            const std::int64_t divisor = unit < 0 ? 1 : kUnitMicro[unit];
            const wire::Width w = widthOf((std::uint64_t(st.dtHi) - std::uint64_t(st.dtLo)) / std::uint64_t(divisor));
            if (w != wire::Width::I64) {
                p.width = w;
                p.base = st.dtLo;
                p.scale = unit == 0   ? wire::Scale::Day
                          : unit == 1 ? wire::Scale::Second
                          : unit == 2 ? wire::Scale::Milli
                                      : wire::Scale::Micro;
            }
        }
        bytes += vectorOf(n, widthBytes(p.width));
    } else if (st.reals) {
        p.encoding = wire::Encoding::Real;
        p.f32 = options.narrowing() && st.realsFitFloat;
        bytes += vectorOf(n, p.f32 ? 4 : 8);
    } else if (st.texts) {
        const std::size_t d = c.dict.size();
        p.encoding = wire::Encoding::Text;
        if (c.dictAlive && d > 0) {
            const std::size_t codeBytes = d <= 256 ? 1 : 2;
            const std::uint64_t dictSize = 4 * (d + 1) + st.dictTextBytes + codeBytes * n;
            const std::uint64_t plainSize = 4 * (n + 1) + c.arena.size();
            if (dictSize * 10 < plainSize * 8) {
                p.encoding = wire::Encoding::TextDict;
                p.width = codeBytes == 1 ? wire::Width::U8 : wire::Width::U16;
            }
        }
        if (p.encoding == wire::Encoding::TextDict)
            bytes += vectorOf(n, widthBytes(p.width)) + vectorOf(d + 1, 4) + vectorOf(st.dictTextBytes, 1);
        else
            bytes += vectorOf(n + 1, 4) + vectorOf(c.arena.size(), 1);
    } else {
        p.encoding = wire::Encoding::Blob;
        bytes += vectorOf(n + 1, 4) + vectorOf(c.arena.size(), 1);
        if (st.anyBlobCut)
            bytes += vectorOf(n, 4);
    }
    p.bytes = bytes;
    return p;
}

/// Writes `n` elements through `fill(i)` into a fresh uninitialized vector.
template <class List, class Fill>
void fillList(List list, std::size_t n, Fill fill)
{
    auto* out = list.data();
    for (std::size_t i = 0; i < n; ++i)
        out[i] = fill(i);
}

void pack(wire::Column::Builder cb, const ColumnPlan& p, const ColumnStage& c, std::size_t n)
{
    namespace bufs = ::bufs;
    const ColumnStats& st = c.stats;
    cb.setEncoding(p.encoding);
    cb.setWidth(p.width);
    cb.setScale(p.scale);
    cb.setBase(p.base);
    cb.setNullCount(st.nulls);
    const std::uint8_t* tags = c.tags.data();
    const std::uint64_t* words = c.words.data();
    const std::uint8_t null = std::uint8_t(Type::Null);
    if (st.nulls) {
        auto bits = cb.initNulls((n + 7) / 8);
        std::uint8_t* b = bits.data();
        for (std::size_t i = 0; i < n; ++i)
            if (tags[i] == null)
                b[i >> 3] = std::uint8_t(b[i >> 3] | (1u << (i & 7)));
    }
    auto bytesOf = [&](auto setter) {
        if (!c.arena.empty())
            (cb.*setter)(c.arena.data(), c.arena.size());
    };

    switch (p.encoding) {
        case wire::Encoding::Empty:
            break;
        case wire::Encoding::Int:
        case wire::Encoding::Datetime: {
            const bool dt = p.encoding == wire::Encoding::Datetime;
            const std::uint64_t base = std::uint64_t(p.base);
            const std::uint64_t unit =
                !dt ? 1
                    : p.scale == wire::Scale::Day      ? std::uint64_t(kUnitMicro[0])
                      : p.scale == wire::Scale::Second ? std::uint64_t(kUnitMicro[1])
                      : p.scale == wire::Scale::Milli  ? std::uint64_t(kUnitMicro[2])
                                                       : 1;
            auto delta = [&](std::size_t i) { return tags[i] == null ? std::uint64_t(0) : (words[i] - base) / unit; };
            switch (p.width) {
                case wire::Width::U8:
                    fillList(cb.initU8(n, bufs::uninitialized), n, [&](std::size_t i) { return std::uint8_t(delta(i)); });
                    break;
                case wire::Width::U16:
                    fillList(cb.initU16(n, bufs::uninitialized), n, [&](std::size_t i) { return std::uint16_t(delta(i)); });
                    break;
                case wire::Width::U32:
                    fillList(cb.initU32(n, bufs::uninitialized), n, [&](std::size_t i) { return std::uint32_t(delta(i)); });
                    break;
                default:
                    fillList(cb.initI64(n, bufs::uninitialized), n,
                             [&](std::size_t i) { return tags[i] == null ? std::int64_t(0) : std::int64_t(words[i]); });
                    break;
            }
            break;
        }
        case wire::Encoding::Real:
            if (p.f32) {
                fillList(cb.initF32(n, bufs::uninitialized), n, [&](std::size_t i) {
                    double d;
                    std::memcpy(&d, &words[i], 8);
                    return tags[i] == null ? 0.0f : float(d);
                });
            } else {
                fillList(cb.initF64(n, bufs::uninitialized), n, [&](std::size_t i) {
                    double d;
                    std::memcpy(&d, &words[i], 8);
                    return tags[i] == null ? 0.0 : d;
                });
            }
            break;
        case wire::Encoding::Text:
        case wire::Encoding::Blob: {
            auto offsets = cb.initOffsets(n + 1, bufs::uninitialized);
            std::uint32_t* o = offsets.data();
            std::uint32_t pos = 0;
            o[0] = 0;
            for (std::size_t i = 0; i < n; ++i) {
                if (tags[i] != null)
                    pos = std::uint32_t(words[i]);
                o[i + 1] = pos;
            }
            bytesOf(&wire::Column::Builder::setBytes);
            if (p.encoding == wire::Encoding::Blob && st.anyBlobCut) {
                std::size_t j = 0;
                fillList(cb.initBlobSizes(n, bufs::uninitialized), n,
                         [&](std::size_t i) { return tags[i] == null ? std::uint32_t(0) : c.fullSizes[j++]; });
            }
            break;
        }
        case wire::Encoding::TextDict: {
            const std::uint16_t* codes = c.codes.data();
            if (p.width == wire::Width::U8)
                fillList(cb.initU8(n, bufs::uninitialized), n, [&](std::size_t i) { return std::uint8_t(codes[i]); });
            else
                fillList(cb.initU16(n, bufs::uninitialized), n, [&](std::size_t i) { return codes[i]; });
            const std::size_t d = c.dict.size();
            auto offsets = cb.initDictOffsets(d + 1, bufs::uninitialized);
            auto dictBytes = cb.initDictBytes(st.dictTextBytes, bufs::uninitialized);
            std::uint32_t* o = offsets.data();
            std::uint8_t* out = dictBytes.data();
            std::uint32_t pos = 0;
            o[0] = 0;
            for (std::size_t i = 0; i < d; ++i) {
                if (c.dict.length(i))
                    std::memcpy(out + pos, c.arena.data() + c.dict.start(i), c.dict.length(i));
                pos += c.dict.length(i);
                o[i + 1] = pos;
            }
            break;
        }
        case wire::Encoding::Mixed: {
            const std::size_t ni = st.ints + st.datetimes, nr = st.reals, k = std::size_t(st.texts) + st.blobs;
            std::uint8_t* t = cb.initTags(n, bufs::uninitialized).data();
            std::memcpy(t, tags, n);
            if (ni) {
                std::int64_t* out = cb.initI64(ni, bufs::uninitialized).data();
                for (std::size_t i = 0; i < n; ++i)
                    if (tags[i] == std::uint8_t(Type::Integer) || tags[i] == std::uint8_t(Type::Datetime))
                        *out++ = std::int64_t(words[i]);
            }
            if (nr) {
                double* out = cb.initF64(nr, bufs::uninitialized).data();
                for (std::size_t i = 0; i < n; ++i)
                    if (tags[i] == std::uint8_t(Type::Real))
                        std::memcpy(out++, &words[i], 8);
            }
            if (k) {
                std::uint32_t* o = cb.initOffsets(k + 1, bufs::uninitialized).data();
                std::size_t j = 0;
                o[0] = 0;
                for (std::size_t i = 0; i < n; ++i)
                    if (tags[i] == std::uint8_t(Type::Text) || tags[i] == std::uint8_t(Type::Blob))
                        o[++j] = std::uint32_t(words[i]);
                bytesOf(&wire::Column::Builder::setBytes);
                if (st.anyBlobCut)
                    cb.setBlobSizes(c.fullSizes.data(), k);
            }
            break;
        }
    }
}

[[noreturn]] void tooBig()
{
    throw Error(ErrorCode::Runtime, "result exceeds 4 GiB in one message; use BufsOptions::batchRows");
}

}  // namespace

struct BufsWriter::Impl
{
    BufsOptions options;
    std::vector<std::string> names, qualified, declTypes;
    std::vector<ColumnStage> columns;
    std::vector<ColumnPlan> plans;
    std::size_t ncols = 0;
    std::size_t staged = 0;
    std::uint64_t seen = 0;
    std::uint64_t emitted = 0;
    std::size_t stagedBytes = 0;  ///< estimate of the staged batch, for batchBytes
    bool truncated = false, begun = false, columnsWritten = false;
    AlignedBuffer out, scratch;
    std::size_t outSize = 0;
    std::function<void(BufsBytes)> sink;

    void reset()
    {
        for (ColumnStage& c : columns)
            c.clear(options.dictionary());
        ncols = staged = 0;
        stagedBytes = 0;
        seen = emitted = 0;
        outSize = 0;
        truncated = begun = columnsWritten = false;
    }

    void begin(const std::vector<std::string>& n, const std::vector<std::string>& q,
               const std::vector<std::string>& d)
    {
        begun = true;
        ncols = n.size();
        names = n;
        qualified = q;
        declTypes = d;
        if (columns.size() < ncols)
            columns.resize(ncols);
        for (std::size_t i = 0; i < ncols; ++i)
            columns[i].clear(options.dictionary());
    }

    /// One row; false when the statement should stop.
    bool addRow(const Value* values, std::size_t count)
    {
        ++seen;
        if (options.maxRows() && seen > options.maxRows()) {
            truncated = true;
            return options.countsAllRows();
        }
        if (staged > 0 && ((options.batchRows() && staged == options.batchRows()) ||
                           (options.batchBytes() && stagedBytes >= options.batchBytes())))
            packBatch(true, 0, 0);
        const std::size_t n = std::min(count, ncols);
        if (options.batchBytes()) {
            for (std::size_t c = 0; c < n; ++c) {
                stagedBytes += 9;
                const Value& v = values[c];
                if (v.type() == Type::Text)
                    stagedBytes += v.text().size();
                else if (v.type() == Type::Blob)
                    stagedBytes += options.blobPrefix() ? std::min(v.blob().size(), options.blobPrefix()) : v.blob().size();
            }
        }
        for (std::size_t c = 0; c < n; ++c)
            columns[c].add(values[c], options);
        ++staged;
        return true;
    }

    void packBatch(bool more, std::uint64_t changes, std::int64_t lastInsertId)
    {
        using Info = wire::ColumnInfo;
        namespace bufs = ::bufs;
        const std::size_t n = staged;
        plans.resize(ncols);
        std::size_t bound = bufs::kHeaderBytes + bufs::tableBytes(wire::ResultSet::kDataBytes, wire::ResultSet::kRefCount) + 16;
        const bool withInfo = !columnsWritten && ncols > 0;
        if (withInfo) {
            bound += bufs::vectorBytes(ncols, 4, 4);
            for (std::size_t i = 0; i < ncols; ++i) {
                bound += bufs::tableBytes(Info::kDataBytes, Info::kRefCount) + bufs::stringBytes(names[i].size()) +
                         bufs::stringBytes(i < qualified.size() ? qualified[i].size() : 0) +
                         bufs::stringBytes(i < declTypes.size() ? declTypes[i].size() : 0);
            }
        }
        if (ncols)
            bound += bufs::vectorBytes(ncols, 4, 4);
        for (std::size_t i = 0; i < ncols; ++i) {
            plans[i] = decide(columns[i], n, options);
            bound += plans[i].bytes;
        }
        if (bound > bufs::kMaxMessageBytes)
            tooBig();

        AlignedBuffer& dest = sink ? scratch : out;
        const std::size_t at = sink ? 0 : outSize;
        dest.reserve(at + bound, at);
        std::size_t size = 0;
        try {
            bufs::MessageBuilder mb(dest.data() + at, bound);
            auto root = mb.initRoot<wire::ResultSet>();
            root.setVersion(1);
            root.setRowOffset(emitted);
            root.setRowCount(std::uint32_t(n));
            root.setMore(more);
            if (!more) {
                root.setTotalRows(seen);
                root.setTruncated(truncated);
                root.setChanges(changes);
                root.setLastInsertId(lastInsertId);
            }
            if (withInfo) {
                auto infos = root.initColumns(ncols);
                for (std::size_t i = 0; i < ncols; ++i) {
                    auto info = infos.init(i);
                    info.setName(names[i]);
                    info.setQualified(i < qualified.size() ? std::string_view(qualified[i]) : std::string_view());
                    info.setDeclType(i < declTypes.size() ? std::string_view(declTypes[i]) : std::string_view());
                }
            }
            if (ncols) {
                auto data = root.initData(ncols);
                for (std::size_t i = 0; i < ncols; ++i)
                    pack(data.init(i), plans[i], columns[i], n);
            }
            size = mb.finish().second;
        } catch (const bufs::CapacityError&) {
            tooBig();
        }
        if (sink)
            sink(BufsBytes(dest.data(), size));
        else
            outSize += size;
        emitted += n;
        staged = 0;
        stagedBytes = 0;
        if (ncols)
            columnsWritten = true;
        for (std::size_t i = 0; i < ncols; ++i)
            columns[i].clear(options.dictionary());
    }
};

BufsWriter::BufsWriter() : impl_(std::make_unique<Impl>()) {}

BufsWriter::BufsWriter(BufsOptions options) : impl_(std::make_unique<Impl>())
{
    impl_->options = options;
}

BufsWriter::~BufsWriter() = default;
BufsWriter::BufsWriter(BufsWriter&&) noexcept = default;
BufsWriter& BufsWriter::operator=(BufsWriter&&) noexcept = default;

const BufsOptions& BufsWriter::options() const noexcept
{
    return impl_->options;
}

BufsWriter& BufsWriter::options(BufsOptions options)
{
    impl_->options = options;
    return *this;
}

BufsBytes BufsWriter::exec(Database& db, std::string_view text, const Params& params)
{
    impl_->reset();
    db.exec(text, params, *this);
    return bytes();
}

BufsBytes BufsWriter::exec(Database& db, std::string_view text, const NamedParams& params)
{
    impl_->reset();
    db.exec(text, params, *this);
    return bytes();
}

BufsBytes BufsWriter::exec(Transaction& txn, std::string_view text, const Params& params)
{
    impl_->reset();
    txn.exec(text, params, *this);
    return bytes();
}

BufsBytes BufsWriter::exec(Statement& stmt)
{
    impl_->reset();
    stmt.exec(*this);
    return bytes();
}

BufsBytes BufsWriter::exec(Statement& stmt, Transaction& txn)
{
    impl_->reset();
    stmt.exec(txn, *this);
    return bytes();
}

BufsWriter& BufsWriter::reset()
{
    impl_->reset();
    return *this;
}

BufsBytes BufsWriter::write(const Result& result)
{
    Impl& m = *impl_;
    m.reset();
    if (!result.columns().empty()) {
        const std::vector<std::string> blank(result.columns().size());
        m.begin(result.columns(), result.qualifiedColumns(), blank);
        for (const Row& row : result)
            if (!m.addRow(row.begin(), row.size()))
                break;
    }
    m.packBatch(false, result.changes(), result.lastInsertId());
    return bytes();
}

BufsWriter& BufsWriter::onBatch(std::function<void(BufsBytes)> sink)
{
    impl_->sink = std::move(sink);
    return *this;
}

BufsBytes BufsWriter::bytes() const noexcept
{
    return BufsBytes(impl_->out.data(), impl_->outSize);
}

std::size_t BufsWriter::capacity() const noexcept
{
    const Impl& m = *impl_;
    std::size_t total = m.out.capacity() + m.scratch.capacity();
    for (const ColumnStage& c : m.columns)
        total += c.capacity();
    return total;
}

void BufsWriter::compact()
{
    Impl& m = *impl_;
    m.out.release();
    m.scratch.release();
    m.outSize = 0;
    for (ColumnStage& c : m.columns)
        c.release();
    releaseVector(m.columns);
    releaseVector(m.plans);
    m.reset();
}

void BufsWriter::begin(const ResultShape& shape)
{
    impl_->begin(shape.names, shape.qualified, shape.declTypes);
}

bool BufsWriter::row(const RowView& row)
{
    return impl_->addRow(row.begin(), row.size());
}

bool BufsWriter::row(const Value* values, std::size_t count)
{
    return impl_->addRow(values, count);
}

void BufsWriter::end(std::uint64_t changes, std::int64_t lastInsertId)
{
    impl_->packBatch(false, changes, lastInsertId);
}

namespace wire {

sql::Result toResult(const ResultView& view)
{
    internal::ResultBuilder rb;
    rb.columns(view.columns(), view.qualifiedColumns());
    const std::size_t width = view.columns().size();
    for (std::uint64_t r = 0; r < view.rowCount(); ++r) {
        std::vector<Value> values;
        values.reserve(width);
        for (std::size_t c = 0; c < width; ++c) {
            Value v = view.value(r, c);
            v.own();
            values.push_back(std::move(v));
        }
        rb.row(std::move(values));
    }
    rb.changed(view.changes());
    rb.lastInsertId(view.lastInsertId());
    return std::move(rb.own);
}

}  // namespace wire

std::vector<std::uint8_t> execBufs(Database& db, std::string_view text, const Params& params,
                                   const BufsOptions& options)
{
    BufsWriter writer(options);
    const BufsBytes b = writer.exec(db, text, params);
    return std::vector<std::uint8_t>(b.begin(), b.end());
}

}  // namespace sql
