// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Reading a source: choosing an access path for a table from the conditions
// that constrain it, and the iterators that fill a source's slots one row at
// a time.
//
// A plan is only ever allowed to be too wide: every candidate row is checked
// against the full WHERE and ON clauses afterwards. So a bound that cannot be
// encoded exactly (a value that the index's collation or affinity would see
// differently) is dropped rather than approximated.

#include <algorithm>
#include <cmath>
#include <limits>

#include "internal/encoding.hpp"
#include "internal/util.hpp"
#include "internal/exec.hpp"

namespace sql::internal {

namespace {

BinaryOp mirror(BinaryOp op) noexcept
{
    switch (op) {
        case BinaryOp::Lt: return BinaryOp::Gt;
        case BinaryOp::Le: return BinaryOp::Ge;
        case BinaryOp::Gt: return BinaryOp::Lt;
        case BinaryOp::Ge: return BinaryOp::Le;
        default: return op;
    }
}

/// Every slot `e` reads already holds a value, and it has no correlated
/// sub-query or non-deterministic call.
bool ready(const Expr* e, const ReadySet& limit)
{
    if (!e)
        return true;
    switch (e->kind) {
        case ExprKind::Column:
        case ExprKind::Slot:
            if (!limit.has(std::size_t(e->slot)))
                return false;
            break;
        case ExprKind::Aggregate:
        case ExprKind::Window:
        case ExprKind::Raise: return false;
        case ExprKind::Subquery:
        case ExprKind::Exists:
        case ExprKind::InSelect:
            if (e->sub && e->sub->correlated)
                return false;
            break;
        case ExprKind::Function:
            if (e->fn && !(e->fn->flags & kDeterministic))
                return false;
            break;
        default: break;
    }
    if (!ready(e->lhs.get(), limit) || !ready(e->rhs.get(), limit) || !ready(e->extra.get(), limit))
        return false;
    for (const ExprPtr& x : e->list) {
        if (!ready(x.get(), limit))
            return false;
    }
    return true;
}

struct Bound
{
    const Expr* value = nullptr;
    Affinity affinity = Affinity::Blob;
    const Expr* term = nullptr;  ///< the conjunct it came from
    bool strict = false;
};

struct ColumnTerms
{
    Bound equal, low, high;
    const Expr* inList = nullptr;
    Affinity inAffinity = Affinity::Blob;
};

/// Collation and direction of the key part a column occupies.
struct PartInfo
{
    int column;
    Collation collation;
    bool desc;
};

/// May a comparison between column `col` and `value` use an index on the
/// column: the comparison must not convert the column's values, and must
/// compare them under the part's collation.
bool usable(const Expr& col, const Expr& value, Collation comparison, Collation part, Affinity& valueAffinity)
{
    if (comparisonAffinity(col, value) != Affinity::Blob)
        return false;
    if (col.strictDatetime)
        return false;
    if (comparison != part)
        return false;
    valueAffinity = comparisonAffinity(value, col);
    return true;
}

}  // namespace

void planAccess(SourceNode& s, const std::vector<Expr*>& conjuncts, const ReadySet& readyBelow)
{
    AccessPath& ap = s.access;
    ap = AccessPath();
    const Table& t = *s.table;
    const std::size_t width = t.width();
    // Terms per (column, collation): the collation must match the key part,
    // so terms are collected per column and checked against each part.
    struct Term
    {
        int column;
        BinaryOp op;
        const Expr* colExpr;
        const Expr* value;
        Collation coll;
        const Expr* inList;
        const Expr* between;
        const Expr* conj;
    };
    std::vector<Term> terms;
    for (const Expr* c : conjuncts) {
        if (c->kind == ExprKind::Binary) {
            BinaryOp op = BinaryOp(c->op);
            if (op != BinaryOp::Eq && op != BinaryOp::Lt && op != BinaryOp::Le && op != BinaryOp::Gt &&
                op != BinaryOp::Ge)
                continue;
            const Expr* col = c->lhs.get();
            const Expr* val = c->rhs.get();
            if (col->kind != ExprKind::Column || std::size_t(col->slot) < s.base ||
                std::size_t(col->slot) >= s.base + width) {
                std::swap(col, val);
                op = mirror(op);
            }
            if (col->kind != ExprKind::Column || std::size_t(col->slot) < s.base ||
                std::size_t(col->slot) >= s.base + width)
                continue;
            if (val->kind == ExprKind::RowValue || !ready(val, readyBelow))
                continue;
            terms.push_back({int(std::size_t(col->slot) - s.base), op, col, val, c->collation, nullptr, nullptr, c});
        } else if (c->kind == ExprKind::InList && !c->negated) {
            const Expr* col = c->lhs.get();
            if (col->kind != ExprKind::Column || std::size_t(col->slot) < s.base ||
                std::size_t(col->slot) >= s.base + width || c->list.empty())
                continue;
            bool ok = true;
            for (const ExprPtr& x : c->list)
                ok = ok && ready(x.get(), readyBelow) && x->kind != ExprKind::RowValue;
            if (!ok)
                continue;
            terms.push_back({int(std::size_t(col->slot) - s.base), BinaryOp::Eq, col, nullptr,
                             col->collationStrength ? col->collation : Collation::Binary, c, nullptr, c});
        } else if (c->kind == ExprKind::Between && !c->negated) {
            const Expr* col = c->lhs.get();
            if (col->kind != ExprKind::Column || std::size_t(col->slot) < s.base ||
                std::size_t(col->slot) >= s.base + width)
                continue;
            if (!ready(c->list[0].get(), readyBelow) || !ready(c->list[1].get(), readyBelow))
                continue;
            terms.push_back({int(std::size_t(col->slot) - s.base), BinaryOp::Ge, col, c->list[0].get(),
                             comparisonCollation(*col, *c->list[0]), nullptr, c, c});
            terms.push_back({int(std::size_t(col->slot) - s.base), BinaryOp::Le, col, c->list[1].get(),
                             comparisonCollation(*col, *c->list[1]), nullptr, c, c});
        }
    }
    if (terms.empty())
        return;

    const int rowidCol = t.hasRowid() ? int(t.columns.size()) : -1;
    auto norm = [&](int col) { return (t.rowidAlias >= 0 && col == t.rowidAlias) ? rowidCol : col; };
    auto termsFor = [&](int column, Collation coll) {
        column = norm(column);
        const bool anyCollation = column == rowidCol;
        ColumnTerms ct;
        for (const Term& tm : terms) {
            if (norm(tm.column) != column)
                continue;
            if (tm.inList) {
                if (ct.equal.value || ct.inList)
                    continue;
                // Every list value must be usable the same way.
                Affinity a = Affinity::Blob;
                bool ok = true;
                bool first = true;
                for (const ExprPtr& x : tm.inList->list) {
                    Affinity xa = Affinity::Blob;
                    const Collation cmp = comparisonCollation(*tm.colExpr, *x);
                    if (!usable(*tm.colExpr, *x, cmp, anyCollation ? cmp : coll, xa) || (!first && xa != a))
                        ok = false;
                    a = xa;
                    first = false;
                }
                if (ok) {
                    ct.inList = tm.inList;
                    ct.inAffinity = a;
                }
                continue;
            }
            Affinity va = Affinity::Blob;
            if (!usable(*tm.colExpr, *tm.value, tm.coll, anyCollation ? tm.coll : coll, va))
                continue;
            switch (tm.op) {
                case BinaryOp::Eq: ct.equal = {tm.value, va, tm.conj, false}; break;
                case BinaryOp::Gt:
                case BinaryOp::Ge: ct.low = {tm.value, va, tm.conj, tm.op == BinaryOp::Gt}; break;
                default: ct.high = {tm.value, va, tm.conj, tm.op == BinaryOp::Lt}; break;
            }
        }
        return ct;
    };

    // Candidate key paths.
    std::vector<PartInfo> keyParts;
    if (t.keyedByRowid()) {
        keyParts.push_back({rowidCol, Collation::Binary, false});
    } else {
        for (std::size_t i = 0; i < t.primaryKey.size(); ++i)
            keyParts.push_back({t.primaryKey[i], t.pkCollation[i], bool(t.pkDesc[i])});
    }

    struct Candidate
    {
        AccessPath::Kind kind;
        const Index* index;
        std::size_t pinned;  ///< equalities (an IN counts as one)
        bool in;
        bool range;
        bool full;
        std::vector<ColumnTerms> chosen;
    };
    auto evaluate = [&](const std::vector<PartInfo>& parts) {
        Candidate cand{};
        for (const PartInfo& p : parts) {
            if (p.column == -1)
                break;  // expression parts are matched separately
            ColumnTerms ct = termsFor(p.column, p.collation);
            if (ct.equal.value) {
                cand.chosen.push_back(ct);
                ++cand.pinned;
                continue;
            }
            if (ct.inList && !cand.in) {
                cand.chosen.push_back(ct);
                cand.in = true;
                ++cand.pinned;
                continue;
            }
            if (ct.low.value || ct.high.value) {
                cand.chosen.push_back(ct);
                cand.range = true;
            }
            break;
        }
        cand.full = cand.pinned == parts.size();
        return cand;
    };

    Candidate best = evaluate(keyParts);
    best.kind = t.keyedByRowid() ? AccessPath::Kind::Rowid : AccessPath::Kind::Key;
    best.index = nullptr;
    const bool forced = !s.indexedBy.empty();
    if (forced) {
        best = Candidate{};
        best.kind = AccessPath::Kind::Scan;
    }
    if (!best.full || forced) {
        for (const Index* ix : t.indexes) {
            if (!ix->whereSql.empty())
                continue;  // partial indexes are not used for reads
            if (forced && ix->name != s.indexedBy && !equalsNoCase(ix->name, s.indexedBy))
                continue;
            std::vector<PartInfo> parts;
            for (const IndexPart& p : ix->parts)
                parts.push_back({p.column == -2 ? rowidCol : p.column, p.collation, p.desc});
            Candidate c = evaluate(parts);
            c.kind = AccessPath::Kind::Index;
            c.index = ix;
            const bool better = forced || c.pinned > best.pinned ||
                                (c.pinned == best.pinned && c.range && !best.range && best.pinned == 0) ||
                                (c.full && ix->unique && !best.full);
            if (better && (c.pinned > 0 || c.range || forced))
                best = std::move(c);
        }
    }
    if (best.pinned == 0 && !best.range) {
        if (forced)
            throw Error(ErrorCode::InvalidArgument, "no query solution");
        return;
    }
    ap.kind = best.kind;
    ap.index = best.index;
    for (std::size_t i = 0; i < best.chosen.size(); ++i) {
        const ColumnTerms& ct = best.chosen[i];
        if (ct.equal.value) {
            ap.equals.push_back(ct.equal.value);
            ap.equalAffinity.push_back(ct.equal.affinity);
            ap.equalTerms.push_back(ct.equal.term);
        } else if (ct.inList) {
            ap.inList = ct.inList;
            ap.inAffinity = ct.inAffinity;
            // An IN pins this part; further parts may still be pinned, but
            // keep it simple: the IN is the last pinned part.
            break;
        } else {
            ap.low = ct.low.value;
            ap.high = ct.high.value;
            ap.lowStrict = ct.low.strict;
            ap.highStrict = ct.high.strict;
            ap.lowTerm = ct.low.term;
            ap.highTerm = ct.high.term;
            ap.rangeAffinity = ct.low.value ? ct.low.affinity : ct.high.affinity;
            if (ct.low.value && ct.high.value && ct.low.affinity != ct.high.affinity) {
                // Keep only one side when they would encode differently.
                ap.high = nullptr;
                ap.highTerm = nullptr;
            }
            break;
        }
    }
}

int accessRank(const SourceNode& s) noexcept
{
    const AccessPath& ap = s.access;
    if (s.kind != SourceNode::Kind::Table)
        return 0;
    const std::size_t parts = ap.kind == AccessPath::Kind::Index ? ap.index->parts.size()
                              : ap.kind == AccessPath::Kind::Rowid ? 1
                              : ap.kind == AccessPath::Kind::Key   ? s.table->primaryKey.size()
                                                                   : 0;
    const std::size_t pinned = ap.equals.size() + (ap.inList ? 1 : 0);
    switch (ap.kind) {
        case AccessPath::Kind::Scan: return 0;
        case AccessPath::Kind::Rowid:
        case AccessPath::Kind::Key:
            if (pinned == parts)
                return 100;
            return pinned ? 40 + int(pinned) : 10;
        case AccessPath::Kind::Index:
            if (pinned == parts && ap.index->unique)
                return 90;
            return pinned ? 30 + int(pinned) : 5;
    }
    return 0;
}

// ------------------------------------------------------------- reading ---

namespace {

[[noreturn]] void corruptRow()
{
    throw Error(ErrorCode::Internal, "malformed record");
}

/// Encodes one bound value for a key part. Returns false when the value
/// cannot select anything through this part (NULL, or a non-integer for a
/// rowid) -- `noMatch` then says whether that means "no rows at all".
bool encodePart(const Value& raw, Affinity aff, bool rowid, Collation coll, bool desc,
                std::string& out, int rounding, bool& noMatch)
{
    noMatch = false;
    if (raw.isNull()) {
        noMatch = true;
        return false;
    }
    const Value* v = &raw;
    Value conv;
    if (aff != Affinity::Blob) {
        conv = raw;
        applyComparisonAffinity(conv, aff);
        v = &conv;
    }
    if (rowid) {
        std::int64_t r = 0;
        if (v->type() == Type::Integer) {
            r = v->integer();
        } else if (v->type() == Type::Real) {
            const double d = v->real();
            if (std::isnan(d)) {
                noMatch = true;
                return false;
            }
            if (rounding == 0) {
                if (!realIsExactInt(d, r)) {
                    noMatch = true;
                    return false;
                }
            } else if (d >= 9.2233720368547758e18) {
                r = rounding > 0 ? std::numeric_limits<std::int64_t>::max()
                                 : std::numeric_limits<std::int64_t>::max();
            } else if (d <= -9.2233720368547758e18) {
                r = std::numeric_limits<std::int64_t>::min();
            } else {
                r = std::int64_t(rounding > 0 ? std::ceil(d) : std::floor(d));
            }
        } else {
            // Text, blob or datetime never equal an integer rowid; as a range
            // bound they lie wholly above every integer.
            if (rounding == 0)
                noMatch = true;
            return false;
        }
        appendRowidKey(out, r);
        return true;
    }
    appendKey(out, *v, coll, desc);
    return true;
}

PartInfo partAt(const SourceNode& s, std::size_t i)
{
    const Table& t = *s.table;
    if (s.access.kind == AccessPath::Kind::Index) {
        const IndexPart& p = s.access.index->parts[i];
        return {p.column, p.collation, p.desc};
    }
    if (s.access.kind == AccessPath::Kind::Rowid)
        return {int(t.columns.size()), Collation::Binary, false};
    return {t.primaryKey[i], t.pkCollation[i], bool(t.pkDesc[i])};
}

void loadRow(SourceNode& s, Ctx& c, std::string_view key, std::string_view payload)
{
    const Table& t = *s.table;
    Value* row = c.slots + s.base;
    std::int64_t rowid = 0;
    if (t.payloadRowid()) {
        const std::uint64_t z = getVarint(payload);
        rowid = std::int64_t((z >> 1) ^ -(z & 1));
    } else if (t.keyedByRowid()) {
        std::string_view k = key;
        if (k.size() < 8)
            corruptRow();
        rowid = readRowidKey(k);
    }
    if (s.decodeWidth > 0) {
        std::size_t have;
        if (s.split == 2) {
            // Two-phase: what this level's filters read now, the rest in
            // finishRow once the row has passed them.
            have = decodeRowInto(payload, row, s.decodeWidth, s.firstUsed.data());
            s.restRecord = payload;
            s.restPending = true;
        } else {
            have = s.used.size() >= s.decodeWidth ? decodeRowInto(payload, row, s.decodeWidth, s.used.data())
                                                  : decodeRowInto(payload, row, s.decodeWidth);
        }
        // Columns added by ALTER TABLE after this row was written.
        for (std::size_t i = have; i < s.decodeWidth; ++i) {
            const Column& col = t.columns[i];
            if (col.hasDefault && col.defaultConstant)
                row[i] = col.defaultValue;
            else
                row[i].setNull();
        }
    }
    if (t.hasRowid()) {
        row[t.columns.size()].setInteger(rowid);
        if (t.rowidAlias >= 0)
            row[t.rowidAlias].setInteger(rowid);
    }
    // VIRTUAL generated columns, in two passes so one may build on another.
    for (int pass = 0; pass < 2 && !s.generated.empty(); ++pass) {
        const Table& tbl = *s.table;
        for (std::size_t i = 0; i < s.generated.size(); ++i) {
            if (!s.generated[i])
                continue;
            Value v = eval(*s.generated[i], c);
            if (!tbl.strict)
                applyAffinity(v, tbl.columns[i].affinity);
            row[i] = std::move(v);
        }
    }
    if (s.keepRowKey)
        s.rowKey.assign(key.data(), key.size());
}

/// Index-only read: fills the used columns (and the rowid) from an entry.
void loadEntry(SourceNode& s, Ctx& c, std::string_view entry, std::string_view flags)
{
    const Table& t = *s.table;
    Value* row = c.slots + s.base;
    std::string_view in = entry;
    const std::size_t parts = s.access.index->parts.size();
    for (std::size_t i = 0; i < parts; ++i) {
        const int col = s.coverColumns[i];
        if (col < 0) {
            skipKeys(in, 1);
            continue;
        }
        decodeKeyInto(in, row[col]);
        if (i / 8 < flags.size() && (std::uint8_t(flags[i / 8]) >> (i % 8) & 1) &&
            row[col].type() == Type::Integer)
            row[col].setReal(double(row[col].integer()));
    }
    if (s.keepRowKey)
        s.rowKey.assign(in.data(), in.size());
    if (t.keyedByRowid()) {
        std::string_view k = in;
        if (k.size() < 8)
            corruptRow();
        const std::int64_t rowid = readRowidKey(k);
        row[t.columns.size()].setInteger(rowid);
        if (t.rowidAlias >= 0)
            row[t.rowidAlias].setInteger(rowid);
        return;
    }
    for (std::size_t j = 0; j < t.primaryKey.size(); ++j) {
        const int col = s.coverColumns[parts + j];
        if (col < 0) {
            skipKeys(in, 1);
            continue;
        }
        decodeKeyInto(in, row[col]);
        const std::size_t bit = parts + j;
        if (bit / 8 < flags.size() && (std::uint8_t(flags[bit / 8]) >> (bit % 8) & 1) &&
            row[col].type() == Type::Integer)
            row[col].setReal(double(row[col].integer()));
    }
}

/// Whether `ix` holds every column the source reads; on success `cols` maps
/// each entry component to the column it fills.
bool covers(const SourceNode& s, const Index& ix, std::vector<int>& cols)
{
    const Table& t = *s.table;
    if (!ix.whereSql.empty())
        return false;
    cols.clear();
    std::vector<std::uint8_t> have(t.columns.size(), 0);
    auto take = [&](int column, Collation coll) {
        // Folded text cannot be read back; the rowid comes from the row key.
        if (column < 0 || coll != Collation::Binary || have[std::size_t(column)]) {
            cols.push_back(-1);
            return;
        }
        have[std::size_t(column)] = 1;
        cols.push_back(column);
    };
    for (const IndexPart& p : ix.parts)
        take(p.column, p.collation);
    bool rowid = false;
    if (t.keyedByRowid()) {
        rowid = true;
        if (t.rowidAlias >= 0)
            have[std::size_t(t.rowidAlias)] = 1;
    } else {
        for (std::size_t j = 0; j < t.primaryKey.size(); ++j)
            take(t.primaryKey[j], t.pkCollation[j]);
    }
    if (s.needRowid && !rowid)
        return false;
    for (std::size_t col = 0; col < s.used.size() && col < have.size(); ++col) {
        if (s.used[col] && !have[col])
            return false;
    }
    // A column read but never marked (decodeWidth set without the mask) is
    // not safe to assume away.
    return s.used.size() >= s.decodeWidth;
}

bool encodePrefix(SourceNode& s, Ctx& c, std::string& out, bool& noMatch)
{
    out.clear();
    AccessPath& ap = s.access;
    for (std::size_t i = 0; i < ap.equals.size(); ++i) {
        const PartInfo p = partAt(s, i);
        const bool rowid = ap.kind == AccessPath::Kind::Rowid ||
                           (ap.kind == AccessPath::Kind::Index && p.column == int(s.table->columns.size()));
        if (!encodePart(eval(*ap.equals[i], c), ap.equalAffinity[i], rowid && ap.kind == AccessPath::Kind::Rowid,
                        p.collation, p.desc, out, 0, noMatch))
            return false;
    }
    return true;
}

/// One bound of a rowid range as the exact integer it implies: the smallest
/// rowid a low bound admits, the largest a high bound admits. False when the
/// bound is not a number (it is then re-checked per row); `empty` when no
/// rowid can satisfy it.
/// Records the range conjuncts a window enforces. A BETWEEN supplies both
/// bounds, so it is enforced only when the window took both from it.
void enforceRange(SourceNode& s, bool lowExact, bool highExact)
{
    const AccessPath& ap = s.access;
    auto ok = [&](const Expr* t, bool exact) {
        if (!t || !exact)
            return false;
        if (t->kind != ExprKind::Between)
            return true;
        return ap.lowTerm == ap.highTerm && lowExact && highExact;
    };
    const bool low = ok(ap.lowTerm, lowExact);
    const bool high = ok(ap.highTerm, highExact);
    if (low)
        s.enforced.push_back(ap.lowTerm);
    if (high && !(low && ap.highTerm == ap.lowTerm))
        s.enforced.push_back(ap.highTerm);
}

bool rowidBound(const Value& raw, Affinity aff, bool isLow, bool strict, std::int64_t& out, bool& empty)
{
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr double kTwo63 = 9223372036854775808.0;
    if (raw.isNull()) {
        empty = true;
        return false;
    }
    const Value* v = &raw;
    Value conv;
    if (aff != Affinity::Blob) {
        conv = raw;
        applyComparisonAffinity(conv, aff);
        v = &conv;
    }
    if (v->type() == Type::Integer) {
        const std::int64_t i = v->integer();
        if (!strict) {
            out = i;
            return true;
        }
        if (i == (isLow ? kMax : kMin)) {
            empty = true;
            return false;
        }
        out = isLow ? i + 1 : i - 1;
        return true;
    }
    if (v->type() != Type::Real)
        return false;
    const double d = v->real();
    if (std::isnan(d)) {
        empty = true;
        return false;
    }
    if (isLow) {
        const double b = strict ? std::floor(d) + 1 : std::ceil(d);
        if (b >= kTwo63) {
            empty = true;
            return false;
        }
        out = b < -kTwo63 ? kMin : std::int64_t(b);
        return true;
    }
    const double b = strict ? std::ceil(d) - 1 : std::floor(d);
    if (b < -kTwo63) {
        empty = true;
        return false;
    }
    out = b >= kTwo63 ? kMax : std::int64_t(b);
    return true;
}

/// A rowid range: an exact window where the bounds are numbers.
void rowidWindow(SourceNode& s, Ctx& c)
{
    AccessPath& ap = s.access;
    std::int64_t lo = std::numeric_limits<std::int64_t>::min();
    std::int64_t hi = std::numeric_limits<std::int64_t>::max();
    bool empty = false;
    const bool lowExact = ap.low && rowidBound(eval(*ap.low, c), ap.rangeAffinity, true, ap.lowStrict, lo, empty);
    const bool highExact =
        !empty && ap.high && rowidBound(eval(*ap.high, c), ap.rangeAffinity, false, ap.highStrict, hi, empty);
    if (empty || lo > hi) {
        s.exhausted = true;
        return;
    }
    ap.lo.clear();
    if (lowExact)
        appendRowidKey(ap.lo, lo);
    ap.hi.clear();
    if (highExact && hi < std::numeric_limits<std::int64_t>::max())
        appendRowidKey(ap.hi, hi + 1);
    enforceRange(s, lowExact, highExact);
}

/// Fills ap.lo / ap.hi for a window starting with `prefix`, with the range
/// bounds (if any) on part `next`.
void window(SourceNode& s, Ctx& c, const std::string& prefix, std::size_t next)
{
    AccessPath& ap = s.access;
    if (ap.kind == AccessPath::Kind::Rowid) {
        rowidWindow(s, c);
        return;
    }
    ap.lo = prefix;
    ap.hi = prefix;
    bool hasLow = false, hasHigh = false;
    bool exact = false;  // the window is exactly the range the terms select
    if (ap.low || ap.high) {
        const PartInfo p = partAt(s, next);
        const Expr* lowE = p.desc ? ap.high : ap.low;
        const Expr* highE = p.desc ? ap.low : ap.high;
        bool noMatch = false;
        if (lowE)
            hasLow = encodePart(eval(*lowE, c), ap.rangeAffinity, false, p.collation, p.desc, ap.lo, -1, noMatch);
        if (highE)
            hasHigh = encodePart(eval(*highE, c), ap.rangeAffinity, false, p.collation, p.desc, ap.hi, 1, noMatch);
        if (!hasLow)
            ap.lo = prefix;
        // Key order is SQL order for an ascending BINARY part, so the bounds
        // can be made exact: past the value for >, NULLs skipped when only
        // a high bound is given.
        exact = !p.desc && p.collation == Collation::Binary && (hasLow || !lowE) && (hasHigh || !highE);
        if (exact) {
            if (hasLow && ap.lowStrict) {
                successorInto(ap.probe, ap.lo);
                ap.lo.swap(ap.probe);
            } else if (!lowE) {
                ap.probe = prefix;
                appendKey(ap.probe, Value());
                successorInto(ap.lo, ap.probe);
            }
            if (hasHigh && !ap.highStrict) {
                successorInto(ap.probe, ap.hi);
                ap.hi.swap(ap.probe);
            }
        } else if (hasHigh) {
            successorInto(ap.probe, ap.hi);
            ap.hi.swap(ap.probe);
        }
    }
    if (!hasHigh)
        successorInto(ap.hi, prefix);
    if (exact)
        enforceRange(s, hasLow, hasHigh);
}

bool inWindow(const AccessPath& ap, std::string_view key)
{
    return ap.hi.empty() || key < std::string_view(ap.hi);
}

bool seekEntry(SourceNode& s, std::string_view at)
{
    nosql::Slice k, v;
    if (!s.cursor.seek(sliceOf(at), k, v))
        return false;
    s.entryKey = viewOf(k);
    s.entryValue = viewOf(v);
    return true;
}

bool nextEntry(SourceNode& s, bool first)
{
    nosql::Slice k, v;
    if (!(first ? s.cursor.first(k, v) : s.cursor.next(k, v)))
        return false;
    s.entryKey = viewOf(k);
    s.entryValue = viewOf(v);
    return true;
}

/// Positions the source's cursor on its next candidate entry for a windowed
/// or point access; false when exhausted.
bool advanceEntry(SourceNode& s)
{
    AccessPath& ap = s.access;
    for (;;) {
        if (s.exhausted)
            return false;
        if (s.holdEntry) {
            s.holdEntry = false;
            return true;
        }
        if (!s.started) {
            s.started = true;
            if (!seekEntry(s, ap.lo) || !inWindow(ap, s.entryKey)) {
                if (!s.probes.empty() && ++s.probeAt < s.probes.size()) {
                    s.started = false;
                    ap.lo = s.probes[s.probeAt];
                    successorInto(ap.hi, ap.lo);
                    continue;
                }
                s.exhausted = true;
                return false;
            }
            return true;
        }
        if (s.skipComponents) {
            // Past every entry sharing the current distinct prefix.
            std::string_view rest = s.entryKey;
            skipKeys(rest, s.skipComponents);
            successorInto(s.skipKey, s.entryKey.substr(0, s.entryKey.size() - rest.size()));
            if (!s.skipKey.empty() && seekEntry(s, s.skipKey) && inWindow(ap, s.entryKey))
                return true;
        } else if (nextEntry(s, false) && inWindow(ap, s.entryKey)) {
            return true;
        }
        if (!s.probes.empty() && ++s.probeAt < s.probes.size()) {
            s.started = false;
            ap.lo = s.probes[s.probeAt];
            successorInto(ap.hi, ap.lo);
            continue;
        }
        s.exhausted = true;
        return false;
    }
}

}  // namespace

void decideCovering(SourceNode& s)
{
    if (s.coverState != 0)
        return;
    s.coverState = 1;
    if (s.kind != SourceNode::Kind::Table || !s.generated.empty())
        return;
    AccessPath& ap = s.access;
    std::vector<int> cols;
    if (ap.kind == AccessPath::Kind::Index) {
        if (covers(s, *ap.index, cols)) {
            s.coverColumns = std::move(cols);
            s.coverState = 2;
        }
        return;
    }
    if (ap.kind != AccessPath::Kind::Scan || s.notIndexed || !s.indexedBy.empty())
        return;
    // A plain scan reads the narrowest index that holds every column it needs.
    const Index* best = nullptr;
    std::vector<int> bestCols;
    for (const Index* ix : s.table->indexes) {
        if (!covers(s, *ix, cols))
            continue;
        if (!best || ix->parts.size() < best->parts.size()) {
            best = ix;
            bestCols = cols;
        }
    }
    if (!best)
        return;
    ap.kind = AccessPath::Kind::Index;
    ap.index = best;
    s.coverColumns = std::move(bestCols);
    s.coverState = 2;
    // DISTINCT over the index's leading columns: one entry per distinct prefix.
    if (!s.distinctColumns.empty()) {
        std::vector<int> want = s.distinctColumns;
        std::sort(want.begin(), want.end());
        want.erase(std::unique(want.begin(), want.end()), want.end());
        const std::size_t k = want.size();
        if (k <= best->parts.size()) {
            std::vector<int> lead;
            bool exact = true;
            for (std::size_t i = 0; i < k; ++i) {
                lead.push_back(best->parts[i].column);
                exact = exact && s.coverColumns[i] == best->parts[i].column;
            }
            std::sort(lead.begin(), lead.end());
            if (exact && lead == want)
                s.skipComponents = k;
        }
    }
}

void openSource(SourceNode& s, Ctx& c)
{
    s.started = false;
    s.exhausted = false;
    s.point = false;
    s.probeAt = 0;
    s.rowIndex = 0;
    s.keepRowKey = s.join == JoinType::Right || s.join == JoinType::Full;
    s.enforced.clear();
    s.holdEntry = false;
    switch (s.kind) {
        case SourceNode::Kind::Single:
        case SourceNode::Kind::Pseudo:
        case SourceNode::Kind::Recursive: return;
        case SourceNode::Kind::Rows: runSubquery(*s.rows, c); return;
        case SourceNode::Kind::Function: {
            if (!s.fnCursor)
                s.fnCursor = s.function->make();
            s.argv.resize(s.args.size());
            for (std::size_t i = 0; i < s.args.size(); ++i)
                s.argv[i] = &eval(*s.args[i], c);
            s.fnCursor->open(*c.fctx, int(s.argv.size()), s.argv.data());
            return;
        }
        case SourceNode::Kind::Table: break;
    }
    decideCovering(s);
    AccessPath& ap = s.access;
    Catalog& cat = c.session->catalog(s.schemaIndex);
    s.rowsDb = &cat.rows(*s.table);
    const nosql::Db& db = ap.kind == AccessPath::Kind::Index ? cat.entries(*ap.index) : cat.rows(*s.table);
    // Keep the cursor from the last open when it still reads this database
    // in this transaction: a join's next probe usually lands near the last.
    if (!s.cursor.boundTo(db))
        s.cursor = db.cursor();
    s.probes.clear();
    if (ap.kind == AccessPath::Kind::Scan) {
        ap.lo.clear();
        ap.hi.clear();
        return;
    }
    bool noMatch = false;
    if (!encodePrefix(s, c, ap.prefix, noMatch)) {
        if (noMatch) {
            s.exhausted = true;
            return;
        }
        // A pinned value that cannot be encoded: fall back to a scan.
        s.cursor = cat.rows(*s.table).cursor();
        ap.lo.clear();
        ap.hi.clear();
        s.point = false;
        s.probes.clear();
        s.started = false;
        s.rowIndex = std::size_t(-1);  // marks "scan the table instead"
        return;
    }
    // Every pinned part matched the key exactly: equality on a key or index
    // part under the comparison's own collation and affinity.
    for (const Expr* t : ap.equalTerms)
        s.enforced.push_back(t);
    const std::size_t pinned = ap.equals.size();
    if (ap.inList) {
        const PartInfo p = partAt(s, pinned);
        const bool rowid = ap.kind == AccessPath::Kind::Rowid;
        for (const ExprPtr& item : ap.inList->list) {
            std::string key = ap.prefix;
            bool nm = false;
            if (encodePart(eval(*item, c), ap.inAffinity, rowid, p.collation, p.desc, key, 0, nm))
                s.probes.push_back(std::move(key));
        }
        std::sort(s.probes.begin(), s.probes.end());
        s.probes.erase(std::unique(s.probes.begin(), s.probes.end()), s.probes.end());
        if (s.probes.empty()) {
            s.exhausted = true;
            return;
        }
        ap.lo = s.probes[0];
        successorInto(ap.hi, ap.lo);
        return;
    }
    const std::size_t parts = ap.kind == AccessPath::Kind::Index ? ap.index->parts.size()
                              : ap.kind == AccessPath::Kind::Rowid ? 1
                                                                   : s.table->primaryKey.size();
    if (ap.kind != AccessPath::Kind::Index && pinned == parts) {
        s.point = true;
        return;
    }
    window(s, c, ap.prefix, pinned);
}

bool nextSource(SourceNode& s, Ctx& c)
{
    switch (s.kind) {
        case SourceNode::Kind::Pseudo:
        case SourceNode::Kind::Single:
            if (s.started)
                return false;
            s.started = true;
            return true;
        case SourceNode::Kind::Recursive: {
            if (s.started || !s.rows->current)
                return false;
            s.started = true;
            for (std::size_t i = 0; i < s.width; ++i)
                c.slots[s.base + i] = s.rows->current[i];
            return true;
        }
        case SourceNode::Kind::Rows: {
            RowStore& rows = s.rows->rows;
            if (s.rowIndex >= rows.size())
                return false;
            const Value* r = rows.row(s.rowIndex);
            for (std::size_t i = 0; i < s.width; ++i)
                c.slots[s.base + i] = r[i];
            s.rowKey.assign(reinterpret_cast<const char*>(&s.rowIndex), sizeof s.rowIndex);
            ++s.rowIndex;
            return true;
        }
        case SourceNode::Kind::Function: {
            TableFunctionCursor& cur = *s.fnCursor;
            if (s.started)
                cur.next(*c.fctx);
            s.started = true;
            if (cur.eof())
                return false;
            const int hiddenStart = s.function->hiddenStart;
            for (int i = 0; i < int(s.width); ++i) {
                if (i < hiddenStart)
                    cur.column(*c.fctx, i, c.slots[s.base + std::size_t(i)]);
                else if (std::size_t(i - hiddenStart) < s.argv.size())
                    c.slots[s.base + std::size_t(i)] = *s.argv[std::size_t(i - hiddenStart)];
                else
                    c.slots[s.base + std::size_t(i)].setNull();
            }
            const std::int64_t id = cur.rowid();
            s.rowKey.assign(reinterpret_cast<const char*>(&id), sizeof id);
            return true;
        }
        case SourceNode::Kind::Table: break;
    }
    if (s.exhausted)
        return false;
    AccessPath& ap = s.access;
    const bool fallbackScan = s.rowIndex == std::size_t(-1);
    if (ap.kind == AccessPath::Kind::Scan || fallbackScan) {
        bool ok;
        if (s.holdEntry) {
            s.holdEntry = false;
            ok = true;
        } else {
            ok = nextEntry(s, !s.started);
        }
        s.started = true;
        if (!ok) {
            s.exhausted = true;
            return false;
        }
        loadRow(s, c, s.entryKey, s.entryValue);
        return true;
    }
    if (s.point) {
        if (s.started)
            return false;
        s.started = true;
        const auto payload = s.rowsDb->get(sliceOf(ap.prefix));
        if (!payload) {
            s.exhausted = true;
            return false;
        }
        loadRow(s, c, ap.prefix, viewOf(*payload));
        return true;
    }
    if (ap.kind != AccessPath::Kind::Index) {
        if (!advanceEntry(s))
            return false;
        loadRow(s, c, s.entryKey, s.entryValue);
        return true;
    }
    if (s.coverState == 2) {
        if (!advanceEntry(s))
            return false;
        loadEntry(s, c, s.entryKey, s.entryValue);
        return true;
    }
    const nosql::Db& data = *s.rowsDb;
    if (!s.rowCursor.boundTo(data))
        s.rowCursor = data.cursor();
    for (;;) {
        if (!advanceEntry(s))
            return false;
        std::string_view entry = s.entryKey;
        skipKeys(entry, ap.index->parts.size());
        nosql::Slice payload;
        if (!s.rowCursor.seekExact(sliceOf(entry), payload))
            continue;  // an entry without its row: defensive, see design notes
        loadRow(s, c, entry, viewOf(payload));
        return true;
    }
}

void closeSource(SourceNode& s)
{
    s.restPending = false;
    if (s.kind == SourceNode::Kind::Table) {
        s.cursor = nosql::Cursor();
        s.rowCursor = nosql::Cursor();
    }
}

void nullSource(SourceNode& s, Ctx& c)
{
    s.restPending = false;
    for (std::size_t i = 0; i < s.width; ++i)
        c.slots[s.base + i].setNull();
}

std::uint64_t skipEntries(SourceNode& s, Ctx& c, std::uint64_t n)
{
    if (n == 0 || s.kind != SourceNode::Kind::Table || s.exhausted || s.started || s.point || !s.probes.empty() ||
        s.skipComponents)
        return 0;
    AccessPath& ap = s.access;
    const bool scan = ap.kind == AccessPath::Kind::Scan || s.rowIndex == std::size_t(-1);
    Catalog& cat = c.session->catalog(s.schemaIndex);
    const nosql::Db& db = !scan && ap.kind == AccessPath::Kind::Index ? cat.entries(*ap.index) : cat.rows(*s.table);
    const std::uint64_t avail = scan ? db.count() : db.countRange(sliceOf(ap.lo), sliceOf(ap.hi));
    s.started = true;
    if (n >= avail) {
        s.exhausted = true;
        return avail;
    }
    if (!(scan ? nextEntry(s, true) : seekEntry(s, ap.lo)) || !s.cursor.skip(n)) {
        s.exhausted = true;
        return avail;
    }
    s.entryKey = viewOf(s.cursor.key());
    s.entryValue = viewOf(s.cursor.value());
    s.holdEntry = true;
    return n;
}

void finishRow(SourceNode& s, Ctx& c)
{
    if (!s.restPending)
        return;
    s.restPending = false;
    decodeRowInto(s.restRecord, c.slots + s.base, s.decodeWidth, s.restUsed.data(), false);
}

}  // namespace sql::internal
