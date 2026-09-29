// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Statement execution.
//
// Planning is deliberately small: for each table in a statement we look at the
// conjuncts that constrain it and pick the narrowest of a primary-key lookup,
// an index lookup and a full scan. Whatever the plan, every predicate is
// re-checked against the row, so a plan is only ever allowed to be too wide.
// Joins are nested loops, and the inner table gets its own plan with the outer
// tables' columns already bound, which is what turns `ON a.id = b.a_id` into an
// index lookup instead of a second scan.
//
// The hot paths are written for a process that runs the same statements for
// months: evaluation hands back references instead of copies, rows are decoded
// into cells that keep their buffers, keys and payloads are built in strings
// that are reused, and scans drive a recycled cursor directly.

#include "internal/executor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "internal/encoding.hpp"
#include "internal/lexer.hpp"

namespace sql::internal {

/// The plan holders the AST points at; the executor's own types derive from
/// them so a statement can carry its plan without knowing its shape.
struct SelectPlan
{
    virtual ~SelectPlan() = default;
};
struct WritePlan
{
    virtual ~WritePlan() = default;
};

namespace {

using Rowbuf = std::vector<Value>;

[[noreturn]] void constraintFailed(const std::string& what)
{
    throw Error(ErrorCode::ConstraintViolation, what);
}

// ---------------------------------------------------------- evaluation ----

Value numericOperand(const Value& v)
{
    switch (v.type()) {
        case Type::Integer:
        case Type::Real: return v;
        case Type::Datetime: return Value(v.datetime().usec);
        case Type::Text: {
            try {
                return v.cast(Type::Integer);
            } catch (const Error&) {
                return v.cast(Type::Real);
            }
        }
        default: break;
    }
    throw Error(ErrorCode::TypeMismatch,
                std::string("cannot do arithmetic on ") + toString(v.type()));
}

void arithmetic(BinaryOp op, const Value& left, const Value& right, Value& out)
{
    if (left.isNull() || right.isNull()) {
        out.setNull();
        return;
    }
    // Integers and reals are already numeric; only text and datetimes need
    // converting, and those are rare in arithmetic.
    const bool plainLeft = left.type() == Type::Integer || left.type() == Type::Real;
    const bool plainRight = right.type() == Type::Integer || right.type() == Type::Real;
    const Value convertedLeft = plainLeft ? Value() : numericOperand(left);
    const Value convertedRight = plainRight ? Value() : numericOperand(right);
    const Value& a = plainLeft ? left : convertedLeft;
    const Value& b = plainRight ? right : convertedRight;
    if (a.type() == Type::Integer && b.type() == Type::Integer) {
        const std::int64_t x = a.integer(), y = b.integer();
        switch (op) {
            case BinaryOp::Add: out.setInteger(x + y); return;
            case BinaryOp::Sub: out.setInteger(x - y); return;
            case BinaryOp::Mul: out.setInteger(x * y); return;
            case BinaryOp::Div:
                if (y == 0)
                    out.setNull();
                else
                    out.setInteger(x / y);
                return;
            default:
                if (y == 0)
                    out.setNull();
                else
                    out.setInteger(x % y);
                return;
        }
    }
    const double x = a.type() == Type::Real ? a.real() : double(a.integer());
    const double y = b.type() == Type::Real ? b.real() : double(b.integer());
    switch (op) {
        case BinaryOp::Add: out.setReal(x + y); return;
        case BinaryOp::Sub: out.setReal(x - y); return;
        case BinaryOp::Mul: out.setReal(x * y); return;
        case BinaryOp::Div:
            if (y == 0)
                out.setNull();
            else
                out.setReal(x / y);
            return;
        default:
            if (y == 0)
                out.setNull();
            else
                out.setReal(std::fmod(x, y));
            return;
    }
}

bool relation(BinaryOp op, int c) noexcept
{
    switch (op) {
        case BinaryOp::Eq: return c == 0;
        case BinaryOp::Ne: return c != 0;
        case BinaryOp::Lt: return c < 0;
        case BinaryOp::Le: return c <= 0;
        case BinaryOp::Gt: return c > 0;
        default: return c >= 0;
    }
}

char foldCase(char c) noexcept
{
    return char(std::tolower(static_cast<unsigned char>(c)));
}

/// `%` spans anything, `_` spans one character, everything else is literal.
/// ASCII case-insensitive, the way LIKE is usually expected to behave.
bool likeMatch(std::string_view pattern, std::string_view text)
{
    std::size_t p = 0, t = 0, star = std::string_view::npos, retry = 0;
    while (t < text.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '_' || foldCase(pattern[p]) == foldCase(text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '%') {
            star = p++;
            retry = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++retry;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '%')
        ++p;
    return p == pattern.size();
}

/// A literal compared against a typed column is read the way that column is
/// stored, so `at > '2024-01-01'` compares two datetimes rather than ranking a
/// datetime against a string. A value that will not convert is left alone, and
/// then simply fails to match. Returns the value to compare: `v` itself when
/// nothing needed doing, `converted` otherwise.
const Value& withAffinity(const Value& v, Type affinity, Value& converted)
{
    if (affinity == Type::Null || v.isNull() || v.type() == affinity)
        return v;
    try {
        converted = v.cast(affinity);
        return converted;
    } catch (const Error&) {
        return v;
    }
}

Type affinityOf(const Expr& a, const Expr& b) noexcept
{
    if (a.kind == ExprKind::Column)
        return a.columnType;
    if (b.kind == ExprKind::Column)
        return b.columnType;
    return Type::Null;
}

/// The finished aggregates of the group being projected, or null wherever an
/// aggregate has no meaning: a WHERE clause, an INSERT value, a join condition.
using Aggregates = const std::vector<Value>*;

/// Evaluates `e` against `row`. Leaves -- a column, a literal, a parameter, an
/// aggregate -- come back as a reference to where they already live; anything
/// computed lands in `tmp` and the reference points there.
const Value& evaluate(const Expr& e, const Rowbuf& row, const Params& params,
                      Aggregates aggregates, Value& tmp)
{
    switch (e.kind) {
        case ExprKind::Literal: return e.literal;

        case ExprKind::Column:
            if (e.slot < 0 || std::size_t(e.slot) >= row.size())
                throw Error(ErrorCode::NoSuchColumn,
                            "column '" + e.describe() + "' is not available here");
            return row[std::size_t(e.slot)];

        case ExprKind::Parameter:
            if (e.parameter >= params.size())
                throw Error(ErrorCode::InvalidArgument,
                            "statement wants at least " + std::to_string(e.parameter + 1) +
                                " parameters, " + std::to_string(params.size()) + " were bound");
            return params[e.parameter];

        case ExprKind::Aggregate:
            if (!aggregates || e.aggregateSlot < 0)
                throw Error(ErrorCode::InvalidArgument,
                            e.describe() + " is only allowed in SELECT, HAVING and ORDER BY");
            return (*aggregates)[std::size_t(e.aggregateSlot)];

        case ExprKind::Unary: {
            Value inner;
            const Value& v = evaluate(*e.lhs, row, params, aggregates, inner);
            if (v.isNull()) {
                tmp.setNull();
                return tmp;
            }
            switch (e.unary) {
                case UnaryOp::Not: tmp.setInteger(!v.truthy()); return tmp;
                case UnaryOp::Plus: tmp = numericOperand(v); return tmp;
                case UnaryOp::Negate: {
                    const Value n = numericOperand(v);
                    if (n.type() == Type::Integer)
                        tmp.setInteger(-n.integer());
                    else
                        tmp.setReal(-n.real());
                    return tmp;
                }
            }
            tmp.setNull();
            return tmp;
        }

        case ExprKind::Binary:
            switch (e.binary) {
                case BinaryOp::And: {
                    Value ta, tb;
                    const Value& a = evaluate(*e.lhs, row, params, aggregates, ta);
                    if (!a.isNull() && !a.truthy()) {
                        tmp.setInteger(0);
                        return tmp;
                    }
                    const Value& b = evaluate(*e.rhs, row, params, aggregates, tb);
                    if (!b.isNull() && !b.truthy()) {
                        tmp.setInteger(0);
                        return tmp;
                    }
                    if (a.isNull() || b.isNull())
                        tmp.setNull();
                    else
                        tmp.setInteger(1);
                    return tmp;
                }
                case BinaryOp::Or: {
                    Value ta, tb;
                    const Value& a = evaluate(*e.lhs, row, params, aggregates, ta);
                    if (!a.isNull() && a.truthy()) {
                        tmp.setInteger(1);
                        return tmp;
                    }
                    const Value& b = evaluate(*e.rhs, row, params, aggregates, tb);
                    if (!b.isNull() && b.truthy()) {
                        tmp.setInteger(1);
                        return tmp;
                    }
                    if (a.isNull() || b.isNull())
                        tmp.setNull();
                    else
                        tmp.setInteger(0);
                    return tmp;
                }
                case BinaryOp::Concat: {
                    Value ta, tb;
                    const Value& a = evaluate(*e.lhs, row, params, aggregates, ta);
                    const Value& b = evaluate(*e.rhs, row, params, aggregates, tb);
                    if (a.isNull() || b.isNull()) {
                        tmp.setNull();
                        return tmp;
                    }
                    tmp = Value(a.toText() + b.toText());
                    return tmp;
                }
                case BinaryOp::Eq:
                case BinaryOp::Ne:
                case BinaryOp::Lt:
                case BinaryOp::Le:
                case BinaryOp::Gt:
                case BinaryOp::Ge: {
                    Value ta, tb, ca, cb;
                    const Value& a = evaluate(*e.lhs, row, params, aggregates, ta);
                    const Value& b = evaluate(*e.rhs, row, params, aggregates, tb);
                    if (a.isNull() || b.isNull()) {
                        tmp.setNull();
                        return tmp;
                    }
                    const Type affinity = affinityOf(*e.lhs, *e.rhs);
                    const Value& x = withAffinity(a, affinity, ca);
                    const Value& y = withAffinity(b, affinity, cb);
                    tmp.setInteger(relation(e.binary, compare(x, y)));
                    return tmp;
                }
                default: {
                    Value ta, tb;
                    const Value& a = evaluate(*e.lhs, row, params, aggregates, ta);
                    const Value& b = evaluate(*e.rhs, row, params, aggregates, tb);
                    arithmetic(e.binary, a, b, tmp);
                    return tmp;
                }
            }

        case ExprKind::IsNull: {
            Value inner;
            tmp.setInteger(evaluate(*e.lhs, row, params, aggregates, inner).isNull() != e.negated);
            return tmp;
        }

        case ExprKind::InList: {
            Value inner;
            const Value& v = evaluate(*e.lhs, row, params, aggregates, inner);
            if (v.isNull()) {
                tmp.setNull();
                return tmp;
            }
            const Type affinity = e.lhs->columnType;
            bool sawNull = false;
            Value ti, ci;
            for (const ExprPtr& item : e.list) {
                const Value& raw = evaluate(*item, row, params, aggregates, ti);
                const Value& candidate = withAffinity(raw, affinity, ci);
                if (candidate.isNull())
                    sawNull = true;
                else if (compare(v, candidate) == 0) {
                    tmp.setInteger(!e.negated);
                    return tmp;
                }
            }
            if (sawNull)
                tmp.setNull();
            else
                tmp.setInteger(e.negated);
            return tmp;
        }

        case ExprKind::Between: {
            Value inner, tl, th, cl, ch;
            const Value& v = evaluate(*e.lhs, row, params, aggregates, inner);
            const Value& lo = withAffinity(evaluate(*e.list[0], row, params, aggregates, tl),
                                           e.lhs->columnType, cl);
            const Value& hi = withAffinity(evaluate(*e.list[1], row, params, aggregates, th),
                                           e.lhs->columnType, ch);
            if (v.isNull() || lo.isNull() || hi.isNull()) {
                tmp.setNull();
                return tmp;
            }
            tmp.setInteger((compare(v, lo) >= 0 && compare(v, hi) <= 0) != e.negated);
            return tmp;
        }

        case ExprKind::Like: {
            Value tv, tp;
            const Value& v = evaluate(*e.lhs, row, params, aggregates, tv);
            const Value& pattern = evaluate(*e.rhs, row, params, aggregates, tp);
            if (v.isNull() || pattern.isNull()) {
                tmp.setNull();
                return tmp;
            }
            const bool matched =
                v.type() == Type::Text && pattern.type() == Type::Text
                    ? likeMatch(pattern.text(), v.text())
                    : likeMatch(pattern.toText(), v.toText());
            tmp.setInteger(matched != e.negated);
            return tmp;
        }
    }
    tmp.setNull();
    return tmp;
}

bool holds(const Expr* e, const Rowbuf& row, const Params& params, Aggregates aggregates = nullptr)
{
    if (!e)
        return true;
    Value tmp;
    return evaluate(*e, row, params, aggregates, tmp).truthy();
}

// ------------------------------------------------------------ aggregates --

/// One aggregate's running state for one group. SUM keeps both an integer and a
/// real total so that a column of integers sums to an integer.
struct Accumulator
{
    std::int64_t count = 0;  ///< rows for COUNT(*), non-NULL inputs otherwise
    std::int64_t intSum = 0;
    double realSum = 0.0;
    bool sumIsReal = false;
    Value best;  ///< MIN / MAX
};

void accumulate(Accumulator& acc, const Expr& e, const Rowbuf& row, const Params& params)
{
    if (!e.lhs) {  // COUNT(*)
        ++acc.count;
        return;
    }
    Value tmp;
    const Value& v = evaluate(*e.lhs, row, params, nullptr, tmp);
    if (v.isNull())
        return;
    switch (e.aggregate) {
        case AggregateOp::Count: break;
        case AggregateOp::Sum:
        case AggregateOp::Avg: {
            if (v.type() == Type::Integer) {
                acc.intSum += v.integer();
                acc.realSum += double(v.integer());
            } else if (v.type() == Type::Real) {
                acc.sumIsReal = true;
                acc.realSum += v.real();
            } else {
                const Value n = numericOperand(v);
                if (n.type() == Type::Real) {
                    acc.sumIsReal = true;
                    acc.realSum += n.real();
                } else {
                    acc.intSum += n.integer();
                    acc.realSum += double(n.integer());
                }
            }
            break;
        }
        case AggregateOp::Min:
            if (acc.count == 0 || compare(v, acc.best) < 0)
                acc.best = v;
            break;
        case AggregateOp::Max:
            if (acc.count == 0 || compare(v, acc.best) > 0)
                acc.best = v;
            break;
    }
    ++acc.count;
}

Value finish(const Accumulator& acc, AggregateOp op)
{
    switch (op) {
        case AggregateOp::Count: return Value(acc.count);
        case AggregateOp::Sum:
            if (acc.count == 0)
                return Value();
            return acc.sumIsReal ? Value(acc.realSum) : Value(acc.intSum);
        case AggregateOp::Avg:
            return acc.count == 0 ? Value() : Value(acc.realSum / double(acc.count));
        case AggregateOp::Min:
        case AggregateOp::Max: return acc.best;
    }
    return Value();
}

bool containsAggregate(const Expr* e)
{
    if (!e)
        return false;
    if (e->kind == ExprKind::Aggregate)
        return true;
    if (containsAggregate(e->lhs.get()) || containsAggregate(e->rhs.get()))
        return true;
    for (const ExprPtr& child : e->list) {
        if (containsAggregate(child.get()))
            return true;
    }
    return false;
}

/// Numbers every aggregate in the tree, left to right, and stops descending at
/// each one: an aggregate's argument is per-row, not per-group.
void collectAggregates(Expr* e, std::vector<Expr*>& out)
{
    if (!e)
        return;
    if (e->kind == ExprKind::Aggregate) {
        if (containsAggregate(e->lhs.get()))
            throw Error(ErrorCode::Unsupported, "aggregate functions cannot be nested");
        e->aggregateSlot = int(out.size());
        out.push_back(e);
        return;
    }
    collectAggregates(e->lhs.get(), out);
    collectAggregates(e->rhs.get(), out);
    for (const ExprPtr& child : e->list)
        collectAggregates(child.get(), out);
}

// -------------------------------------------------------------- binding ---

/// One table participating in a statement, with the slot range it owns.
struct SourcePlan
{
    const Table* table = nullptr;
    std::string alias;
    std::size_t base = 0;
    Expr* on = nullptr;

    enum class Kind
    {
        Scan,
        Key,   ///< through the primary key: a point lookup when every key
               ///< column is pinned, a window over the rows otherwise
        Index,
    };
    Kind kind = Kind::Scan;
    const Index* index = nullptr;
    std::vector<const Expr*> equals;  ///< leading key or index columns
    /// Bounds on the column after the pinned ones.
    const Expr* low = nullptr;
    const Expr* high = nullptr;

    /// Per-scan scratch: the encoded bounds live here while the scan -- and
    /// any scan nested inside it -- runs.
    std::string prefix, lo, hi;
};

int resolveColumn(const std::vector<SourcePlan>& sources, const std::string& alias,
                  const std::string& column)
{
    int found = -1;
    for (const SourcePlan& s : sources) {
        if (!alias.empty() && !equalsNoCase(alias, s.alias) && !equalsNoCase(alias, s.table->name))
            continue;
        const int local = s.table->find(column);
        if (local < 0)
            continue;
        if (found >= 0)
            throw Error(ErrorCode::AmbiguousColumn,
                        "column '" + column + "' is in more than one table");
        found = int(s.base) + local;
    }
    if (found < 0) {
        const std::string what = alias.empty() ? column : alias + "." + column;
        throw Error(ErrorCode::NoSuchColumn, "no such column: " + what);
    }
    return found;
}

/// A literal compared against a column is converted once, here, rather than
/// on every row. One that will not convert is left as it is and compares by
/// class, exactly as the per-row path would have it.
void settleLiteral(Expr& literal, Type affinity)
{
    if (affinity == Type::Null || literal.literal.isNull() || literal.literal.type() == affinity)
        return;
    try {
        literal.literal = literal.literal.cast(affinity);
    } catch (const Error&) {
    }
}

void bindColumns(Expr* e, const std::vector<SourcePlan>& sources)
{
    if (!e)
        return;
    if (e->kind == ExprKind::Column) {
        e->slot = resolveColumn(sources, e->table, e->column);
        for (const SourcePlan& s : sources) {
            const std::size_t slot = std::size_t(e->slot);
            if (slot >= s.base && slot < s.base + s.table->width()) {
                e->columnType = s.table->slotType(int(slot - s.base));
                break;
            }
        }
    }
    bindColumns(e->lhs.get(), sources);
    bindColumns(e->rhs.get(), sources);
    for (const ExprPtr& child : e->list)
        bindColumns(child.get(), sources);

    if (e->kind == ExprKind::Binary && e->binary >= BinaryOp::Eq && e->binary <= BinaryOp::Ge) {
        const Type affinity = affinityOf(*e->lhs, *e->rhs);
        if (e->lhs->kind == ExprKind::Literal)
            settleLiteral(*e->lhs, affinity);
        if (e->rhs->kind == ExprKind::Literal)
            settleLiteral(*e->rhs, affinity);
    } else if ((e->kind == ExprKind::InList || e->kind == ExprKind::Between) &&
               e->lhs->kind == ExprKind::Column) {
        for (const ExprPtr& child : e->list) {
            if (child->kind == ExprKind::Literal)
                settleLiteral(*child, e->lhs->columnType);
        }
    }
}

/// The last table in join order that `e` reads, which is the one FOR JSON AUTO
/// nests the result column under. Literals and aggregates answer 0, the
/// outermost table, and so stay at the top level of the document.
int deepestSource(const Expr* e, const std::vector<SourcePlan>& sources)
{
    if (!e)
        return 0;
    int deepest = 0;
    if (e->kind == ExprKind::Column && e->slot >= 0) {
        const std::size_t slot = std::size_t(e->slot);
        for (std::size_t d = 0; d < sources.size(); ++d) {
            if (slot >= sources[d].base && slot < sources[d].base + sources[d].table->width())
                deepest = int(d);
        }
    }
    deepest = std::max(deepest, deepestSource(e->lhs.get(), sources));
    deepest = std::max(deepest, deepestSource(e->rhs.get(), sources));
    for (const ExprPtr& child : e->list)
        deepest = std::max(deepest, deepestSource(child.get(), sources));
    return deepest;
}

// -------------------------------------------------------------- planning --

void conjuncts(const Expr* e, std::vector<const Expr*>& out)
{
    if (!e)
        return;
    if (e->kind == ExprKind::Binary && e->binary == BinaryOp::And) {
        conjuncts(e->lhs.get(), out);
        conjuncts(e->rhs.get(), out);
        return;
    }
    out.push_back(e);
}

/// True when every column the expression reads is already in the row, which is
/// what makes it usable as a lookup value for the table being planned.
bool readyAt(const Expr& e, std::size_t limit)
{
    if (e.kind == ExprKind::Column)
        return e.slot >= 0 && std::size_t(e.slot) < limit;
    if (e.lhs && !readyAt(*e.lhs, limit))
        return false;
    if (e.rhs && !readyAt(*e.rhs, limit))
        return false;
    for (const ExprPtr& child : e.list) {
        if (!readyAt(*child, limit))
            return false;
    }
    return true;
}

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

void planAccess(SourcePlan& sp, const std::vector<const Expr*>& available)
{
    const Table& t = *sp.table;
    const std::size_t width = t.width();
    std::vector<const Expr*> equal(width, nullptr), low(width, nullptr), high(width, nullptr);

    for (const Expr* c : available) {
        if (c->kind != ExprKind::Binary)
            continue;
        BinaryOp op = c->binary;
        const Expr* column = c->lhs.get();
        const Expr* value = c->rhs.get();
        if (column->kind != ExprKind::Column) {
            std::swap(column, value);
            op = mirror(op);
        }
        if (column->kind != ExprKind::Column || column->slot < 0)
            continue;
        const std::size_t slot = std::size_t(column->slot);
        if (slot < sp.base || slot >= sp.base + width)
            continue;
        if (!readyAt(*value, sp.base))
            continue;
        const std::size_t k = slot - sp.base;
        switch (op) {
            case BinaryOp::Eq: equal[k] = value; break;
            case BinaryOp::Gt:
            case BinaryOp::Ge: low[k] = value; break;
            case BinaryOp::Lt:
            case BinaryOp::Le: high[k] = value; break;
            default: break;
        }
    }

    // How many leading columns of a key or index the equalities pin down.
    const auto pinned = [&](const std::vector<int>& columns) {
        std::size_t n = 0;
        while (n < columns.size() && equal[std::size_t(columns[n])])
            ++n;
        return n;
    };
    const std::vector<int>& keySlots = t.keySlots();
    const auto useKey = [&](std::size_t n) {
        sp.kind = SourcePlan::Kind::Key;
        for (std::size_t i = 0; i < n; ++i)
            sp.equals.push_back(equal[std::size_t(keySlots[i])]);
        if (n < keySlots.size()) {
            sp.low = low[std::size_t(keySlots[n])];
            sp.high = high[std::size_t(keySlots[n])];
        }
    };

    const std::size_t keyPinned = pinned(keySlots);
    if (keyPinned == keySlots.size()) {
        useKey(keyPinned);
        return;
    }

    const Index* best = nullptr;
    std::size_t bestLeading = 0;
    for (const Index& ix : t.indexes) {
        const std::size_t n = pinned(ix.columns);
        if (n > bestLeading) {
            bestLeading = n;
            best = &ix;
        }
    }
    // A run of leading key columns beats an index run of the same length: the
    // rows come straight out of the window instead of through a second lookup.
    if (keyPinned > 0 && keyPinned >= bestLeading) {
        useKey(keyPinned);
        return;
    }
    if (best) {
        sp.kind = SourcePlan::Kind::Index;
        sp.index = best;
        for (std::size_t i = 0; i < bestLeading; ++i)
            sp.equals.push_back(equal[std::size_t(best->columns[i])]);
        if (bestLeading < best->columns.size()) {
            const std::size_t next = std::size_t(best->columns[bestLeading]);
            sp.low = low[next];
            sp.high = high[next];
        }
        return;
    }

    if (low[std::size_t(keySlots[0])] || high[std::size_t(keySlots[0])]) {
        useKey(0);
        return;
    }
    for (const Index& ix : t.indexes) {
        const std::size_t first = std::size_t(ix.columns[0]);
        if (low[first] || high[first]) {
            sp.kind = SourcePlan::Kind::Index;
            sp.index = &ix;
            sp.low = low[first];
            sp.high = high[first];
            return;
        }
    }
}

// ------------------------------------------------------------ row access --

void loadRow(const Table& t, nosql::Slice key, nosql::Slice payload, Rowbuf& row, std::size_t base)
{
    decodeRowInto(viewOf(payload), row.data() + base, t.columns.size());
    if (t.hasRowid()) {
        std::string_view k = viewOf(key);
        decodeKeyInto(k, row[base + t.columns.size()]);
    }
}

/// The key `row` is stored under: its key columns encoded in order. Encoded
/// values concatenate without a separator and still sort like the tuple.
void rowKeyInto(std::string& out, const Table& t, const Rowbuf& row, std::size_t base = 0)
{
    out.clear();
    for (const int slot : t.keySlots())
        appendKey(out, row[base + std::size_t(slot)]);
}

/// A bound that could not be cast to the column's type is simply dropped: the
/// predicate is re-checked on every row, so a wider window stays correct.
bool encodeBound(const Expr* e, Type target, const Rowbuf& row, const Params& params,
                 std::string& out)
{
    if (!e)
        return false;
    Value tmp;
    const Value& v = evaluate(*e, row, params, nullptr, tmp);
    if (v.isNull())
        return false;
    if (v.type() == target || target == Type::Null) {
        appendKey(out, v);
        return true;
    }
    try {
        appendKey(out, v.cast(target));
    } catch (const Error&) {
        return false;
    }
    return true;
}

/// Fills `sp.lo` and `sp.hi` with the byte range holding every entry that
/// starts with `sp.prefix` and whose next encoded column lies within the plan's
/// bounds. An empty `hi` means unbounded.
void keyWindow(SourcePlan& sp, Type nextType, const Rowbuf& row, const Params& params)
{
    sp.lo.assign(sp.prefix);
    sp.hi.assign(sp.prefix);
    const bool hasLow = encodeBound(sp.low, nextType, row, params, sp.lo);
    const bool hasHigh = encodeBound(sp.high, nextType, row, params, sp.hi);
    // 0xff outranks every key tag, so it closes the window just past the last
    // entry that shares the bound's value.
    if (hasHigh)
        sp.hi.push_back('\xff');
    else
        successorInto(sp.hi, sp.prefix);
    if (!hasLow)
        sp.lo.assign(sp.prefix);
}

/// Encodes the values pinning the leading columns, or reports that one of
/// them cannot be (a NULL, or a value the column's type has no room for).
bool encodePrefix(const SourcePlan& sp, const std::vector<int>& columns, const Rowbuf& row,
                  const Params& params, std::string& prefix)
{
    prefix.clear();
    for (std::size_t i = 0; i < sp.equals.size(); ++i) {
        if (!encodeBound(sp.equals[i], sp.table->slotType(columns[i]), row, params, prefix))
            return false;
    }
    return true;
}

/// True when one of the pinned values is NULL, which no row can equal.
bool pinnedToNull(const SourcePlan& sp, const Rowbuf& row, const Params& params)
{
    for (const Expr* e : sp.equals) {
        Value tmp;
        if (evaluate(*e, row, params, nullptr, tmp).isNull())
            return true;
    }
    return false;
}

/// Walks the entries of `db` in [lo, hi), or from `lo` to the end when `hi` is
/// empty, calling `fn(key, value)` until it answers false.
template <class Fn>
bool walkWindow(const nosql::Db& db, std::string_view lo, std::string_view hi, Fn&& fn)
{
    nosql::Cursor cur = db.cursor();
    const nosql::Slice upper = sliceOf(hi);
    for (bool ok = cur.seek(sliceOf(lo)); ok; ok = cur.next()) {
        const nosql::Slice key = cur.key();
        if (!hi.empty() && key.compare(upper) >= 0)
            break;
        if (!fn(key, cur.value()))
            return false;
    }
    return true;
}

template <class Fn>
bool walkAll(const nosql::Db& db, Fn&& fn)
{
    nosql::Cursor cur = db.cursor();
    for (bool ok = cur.first(); ok; ok = cur.next()) {
        if (!fn(cur.key(), cur.value()))
            return false;
    }
    return true;
}

/// Fills `row[sp.base ...]` once per candidate and calls `visit`. Returns false
/// as soon as `visit` asks to stop.
template <class Visit>
bool scanSource(const Catalog& cat, SourcePlan& sp, Rowbuf& row, const Params& params,
                Visit&& visit)
{
    const Table& t = *sp.table;
    const nosql::Db& data = cat.rows(t);

    if (sp.kind == SourcePlan::Kind::Key) {
        const std::vector<int>& keySlots = t.keySlots();
        if (encodePrefix(sp, keySlots, row, params, sp.prefix)) {
            if (sp.equals.size() == keySlots.size()) {
                if (const auto payload = data.get(sliceOf(sp.prefix))) {
                    loadRow(t, sliceOf(sp.prefix), *payload, row, sp.base);
                    return visit();
                }
                return true;
            }
            keyWindow(sp, t.slotType(keySlots[sp.equals.size()]), row, params);
            return walkWindow(data, sp.lo, sp.hi, [&](nosql::Slice key, nosql::Slice payload) {
                loadRow(t, key, payload, row, sp.base);
                return visit();
            });
        }
        // NULL never matches; anything else falls through to a full scan.
        if (pinnedToNull(sp, row, params))
            return true;
    }

    if (sp.kind == SourcePlan::Kind::Index) {
        const Index& ix = *sp.index;
        if (encodePrefix(sp, ix.columns, row, params, sp.prefix)) {
            const std::size_t bounded = sp.equals.size();
            const Type nextType =
                bounded < ix.columns.size() ? t.slotType(ix.columns[bounded]) : Type::Integer;
            keyWindow(sp, nextType, row, params);
            const nosql::Db& entries = cat.entries(ix);
            return walkWindow(entries, sp.lo, sp.hi, [&](nosql::Slice entry, nosql::Slice) {
                std::string_view key = viewOf(entry);
                skipKeys(key, ix.columns.size());
                const auto payload = data.get(sliceOf(key));
                if (!payload)
                    return true;
                loadRow(t, sliceOf(key), *payload, row, sp.base);
                return visit();
            });
        }
    }

    return walkAll(data, [&](nosql::Slice key, nosql::Slice payload) {
        loadRow(t, key, payload, row, sp.base);
        return visit();
    });
}

// ---------------------------------------------------------- constraints ---

std::string describeColumns(const Table& t, const Index& ix)
{
    std::string out;
    for (std::size_t i = 0; i < ix.columns.size(); ++i) {
        if (i)
            out += ", ";
        out += t.name + "." + t.columns[std::size_t(ix.columns[i])].name;
    }
    return out;
}

/// Buffers a statement builds its keys and payloads in, reused row after row.
struct WriteScratch
{
    std::string key, payload, entry;
};

/// `out` receives the entry's bytes: the indexed columns encoded in order,
/// then the row's key. False when one of the indexed columns is NULL.
bool indexEntryInto(std::string& out, const Index& ix, const Rowbuf& row, const std::string& key,
                    std::size_t* prefixSize)
{
    out.clear();
    bool anyNull = false;
    for (const int c : ix.columns) {
        const Value& v = row[std::size_t(c)];
        anyNull = anyNull || v.isNull();
        appendKey(out, v);
    }
    *prefixSize = out.size();
    out.append(key);
    return !anyNull;
}

/// Adds `row`'s entry to one index, enforcing UNIQUE first. SQL lets NULLs
/// repeat in a unique index, so only fully-known tuples are checked.
void addIndexEntry(const Catalog& cat, const Table& t, const Index& ix, const Rowbuf& row,
                   const std::string& key, WriteScratch& ws)
{
    std::size_t prefixSize = 0;
    const bool known = indexEntryInto(ws.entry, ix, row, key, &prefixSize);
    const nosql::Db& entries = cat.entries(ix);
    if (ix.unique && known) {
        // Encoded values are self-delimiting, so every entry that begins with
        // the same bytes carries the same indexed values.
        const nosql::Slice prefix(ws.entry.data(), prefixSize);
        nosql::Cursor cur = entries.cursor();
        for (bool ok = cur.seek(prefix); ok && cur.key().startsWith(prefix); ok = cur.next()) {
            if (cur.key() != sliceOf(ws.entry))
                constraintFailed("UNIQUE constraint failed: " + describeColumns(t, ix));
        }
    }
    entries.put(sliceOf(ws.entry), nosql::Slice());
}

void addIndexEntries(const Catalog& cat, const Table& t, const Rowbuf& row, const std::string& key,
                     WriteScratch& ws)
{
    for (const Index& ix : t.indexes)
        addIndexEntry(cat, t, ix, row, key, ws);
}

void removeIndexEntries(const Catalog& cat, const Table& t, const Rowbuf& row,
                        const std::string& key, WriteScratch& ws)
{
    for (const Index& ix : t.indexes) {
        std::size_t prefixSize = 0;
        indexEntryInto(ws.entry, ix, row, key, &prefixSize);
        cat.entries(ix).erase(sliceOf(ws.entry));
    }
}

/// Casts every column to its declared type and enforces NOT NULL.
void finalizeRow(const Table& t, Rowbuf& row)
{
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        const ColumnInfo& c = t.columns[i];
        Value& v = row[i];
        if (!v.isNull() && v.type() != c.type) {
            try {
                v = v.cast(c.type);
            } catch (const Error& e) {
                throw Error(ErrorCode::TypeMismatch,
                            std::string(e.what()) + " (column " + t.name + "." + c.name + ")");
            }
        }
        if (c.notNull && v.isNull())
            constraintFailed("NOT NULL constraint failed: " + t.name + "." + c.name);
    }
    if (t.hasRowid()) {
        Value& id = row[t.columns.size()];
        if (!id.isNull() && id.type() != Type::Integer)
            id = id.cast(Type::Integer);
    }
    for (const int slot : t.keySlots()) {
        if (row[std::size_t(slot)].isNull())
            constraintFailed("PRIMARY KEY may not be NULL: " + t.name);
    }
}

// ------------------------------------------------------------ statements --

Result runCreateTable(Catalog& cat, CreateTableStmt& s)
{
    ResultBuilder out;
    if (cat.find(s.name)) {
        if (s.ifNotExists)
            return out.result;
        throw Error(ErrorCode::TableExists, "table already exists: " + s.name);
    }
    if (s.columns.empty())
        throw Error(ErrorCode::InvalidArgument, "a table needs at least one column");

    Table t;
    t.name = s.name;
    t.columns = s.columns;
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (equalsNoCase(t.columns[i].name, t.columns[j].name))
                throw Error(ErrorCode::InvalidArgument,
                            "duplicate column name: " + t.columns[i].name);
        }
        if (equalsNoCase(t.columns[i].name, "rowid"))
            throw Error(ErrorCode::InvalidArgument, "'rowid' is reserved");
        if (t.columns[i].primaryKey) {
            if (!t.primaryKey.empty() || !s.tablePrimaryKey.empty())
                throw Error(ErrorCode::InvalidArgument,
                            "a table may declare only one PRIMARY KEY");
            t.primaryKey.push_back(int(i));
        }
        if (t.columns[i].hasDefault)
            t.columns[i].defaultValue = t.columns[i].defaultValue.cast(t.columns[i].type);
    }

    // Names from a table-level constraint, as column positions, each once.
    const auto positions = [&](const std::vector<std::string>& names, const char* what) {
        std::vector<int> slots;
        for (const std::string& name : names) {
            const int slot = t.find(name);
            if (slot < 0 || std::size_t(slot) >= t.columns.size())
                throw Error(ErrorCode::NoSuchColumn,
                            std::string(what) + " names an undeclared column '" + name + "'");
            if (std::find(slots.begin(), slots.end(), slot) != slots.end())
                throw Error(ErrorCode::InvalidArgument,
                            std::string(what) + " lists column '" + name + "' twice");
            slots.push_back(slot);
        }
        return slots;
    };
    if (!s.tablePrimaryKey.empty())
        t.primaryKey = positions(s.tablePrimaryKey, "PRIMARY KEY");
    for (const int c : t.primaryKey) {
        t.columns[std::size_t(c)].primaryKey = true;
        t.columns[std::size_t(c)].notNull = true;
    }
    const auto isKey = [&](const std::vector<int>& columns) {
        return columns.size() == t.primaryKey.size() &&
               std::is_permutation(columns.begin(), columns.end(), t.primaryKey.begin());
    };

    // Every UNIQUE that the key does not already guarantee becomes an index.
    std::vector<Index> uniques;
    const auto unique = [&](std::vector<int> columns) {
        if (isKey(columns))
            return;
        Index ix;
        ix.name = t.name;
        for (const int c : columns)
            ix.name += "_" + t.columns[std::size_t(c)].name;
        ix.name += "_unique";
        ix.table = t.name;
        ix.columns = std::move(columns);
        ix.unique = true;
        for (const Index& seen : uniques) {
            if (equalsNoCase(seen.name, ix.name))
                return;
        }
        if (cat.findIndex(ix.name))
            throw Error(ErrorCode::IndexExists, "index already exists: " + ix.name);
        uniques.push_back(std::move(ix));
    };
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        if (t.columns[i].unique)
            unique({int(i)});
    }
    for (const std::vector<std::string>& names : s.uniques)
        unique(positions(names, "UNIQUE"));

    cat.addTable(std::move(t));
    for (Index& ix : uniques)
        cat.addIndex(std::move(ix));
    return out.result;
}

Result runDropTable(Catalog& cat, DropTableStmt& s)
{
    ResultBuilder out;
    const Table* t = cat.find(s.name);
    if (!t) {
        if (s.ifExists)
            return out.result;
        throw Error(ErrorCode::NoSuchTable, "no such table: " + s.name);
    }
    cat.dropTable(*t);
    return out.result;
}

Result runCreateIndex(Catalog& cat, CreateIndexStmt& s)
{
    ResultBuilder out;
    if (cat.findIndex(s.name)) {
        if (s.ifNotExists)
            return out.result;
        throw Error(ErrorCode::IndexExists, "index already exists: " + s.name);
    }
    {
        const Table& t = cat.get(s.table);
        Index ix;
        ix.name = s.name;
        ix.table = t.name;
        ix.unique = s.unique;
        for (const std::string& column : s.columns) {
            const int slot = t.find(column);
            if (slot < 0 || std::size_t(slot) >= t.columns.size())
                throw Error(ErrorCode::NoSuchColumn, "no such column: " + t.name + "." + column);
            ix.columns.push_back(slot);
        }
        cat.addIndex(std::move(ix));
    }

    // Backfill over the rows already there. The schema was rebuilt by
    // addIndex, so look the table and index up again.
    const Table& t = cat.get(s.table);
    const Index& ix = *cat.findIndex(s.name);
    Rowbuf row(t.width());
    WriteScratch ws;
    walkAll(cat.rows(t), [&](nosql::Slice key, nosql::Slice payload) {
        loadRow(t, key, payload, row, 0);
        rowKeyInto(ws.key, t, row);
        addIndexEntry(cat, t, ix, row, ws.key, ws);
        return true;
    });
    return out.result;
}

Result runDropIndex(Catalog& cat, DropIndexStmt& s)
{
    ResultBuilder out;
    const Index* ix = cat.findIndex(s.name);
    if (!ix) {
        if (s.ifExists)
            return out.result;
        throw Error(ErrorCode::NoSuchIndex, "no such index: " + s.name);
    }
    cat.dropIndex(*ix);
    return out.result;
}

/// What a single-table write statement needs to run, planned once per schema
/// snapshot and kept on the statement.
struct WritePlanImpl : WritePlan
{
    std::shared_ptr<const Schema> schema;
    const Table* table = nullptr;
    std::vector<SourcePlan> sources;  ///< UPDATE and DELETE: the one scanned source
    std::vector<int> targets;         ///< INSERT column slots, or UPDATE assignment slots
    // Scratch reused across runs.
    Rowbuf row;
    std::string oldKey;
    WriteScratch ws;
};

WritePlanImpl& writePlan(std::shared_ptr<WritePlan>& slot, const Catalog& cat, bool& fresh)
{
    auto* have = static_cast<WritePlanImpl*>(slot.get());
    fresh = !have || have->schema != cat.schema();
    if (fresh) {
        auto made = std::make_shared<WritePlanImpl>();
        made->schema = cat.schema();
        slot = made;
        return *made;
    }
    return *have;
}

Result runInsert(Catalog& cat, InsertStmt& s, const Params& params)
{
    ResultBuilder out;
    bool fresh = false;
    WritePlanImpl& plan = writePlan(s.plan, cat, fresh);
    if (fresh) {
        const Table& t = cat.get(s.table);
        plan.table = &t;
        if (s.columns.empty()) {
            for (std::size_t i = 0; i < t.columns.size(); ++i)
                plan.targets.push_back(int(i));
        } else {
            for (const std::string& name : s.columns) {
                const int slot = t.find(name);
                if (slot < 0)
                    throw Error(ErrorCode::NoSuchColumn, "no such column: " + t.name + "." + name);
                plan.targets.push_back(slot);
            }
        }
        plan.row.resize(t.width());
    }
    const Table& t = *plan.table;
    const std::vector<int>& targets = plan.targets;
    Rowbuf& row = plan.row;
    WriteScratch& ws = plan.ws;

    const nosql::Db& data = cat.rows(t);
    const Rowbuf empty;
    std::uint64_t inserted = 0;
    std::int64_t lastId = 0;
    Value tmp;

    for (std::vector<ExprPtr>& values : s.rows) {
        if (values.size() != targets.size())
            throw Error(ErrorCode::InvalidArgument,
                        "INSERT has " + std::to_string(values.size()) + " values for " +
                            std::to_string(targets.size()) + " columns");

        for (std::size_t i = 0; i < t.columns.size(); ++i) {
            if (t.columns[i].hasDefault)
                row[i] = t.columns[i].defaultValue;
            else
                row[i].setNull();
        }
        if (t.hasRowid())
            row[t.columns.size()].setNull();
        for (std::size_t i = 0; i < targets.size(); ++i)
            row[std::size_t(targets[i])] = evaluate(*values[i], empty, params, nullptr, tmp);

        Value& id = row[std::size_t(t.autoKeySlot())];
        if (t.autoKey() && id.isNull())
            id.setInteger(cat.nextRowid(t));
        finalizeRow(t, row);
        if (t.autoKey())
            cat.noteRowid(t, id.integer());

        rowKeyInto(ws.key, t, row);
        ws.payload.clear();
        encodeRowInto(ws.payload, row.data(), t.columns.size());
        if (!data.put(sliceOf(ws.key), sliceOf(ws.payload), nosql::PutMode::InsertUnique))
            constraintFailed("PRIMARY KEY must be unique: " + t.name);
        addIndexEntries(cat, t, row, ws.key, ws);

        ++inserted;
        if (t.autoKey())
            lastId = id.integer();
    }

    out.changed(inserted);
    out.lastInsertId(lastId);
    return out.result;
}

/// Materialises every row a single-table statement touches, so that the writes
/// that follow cannot disturb the scan producing them.
std::vector<Rowbuf> collectRows(const Catalog& cat, SourcePlan& sp, Rowbuf& row, const Expr* where,
                                const Params& params)
{
    std::vector<Rowbuf> out;
    scanSource(cat, sp, row, params, [&] {
        if (holds(where, row, params))
            out.push_back(row);
        return true;
    });
    return out;
}

void singleSource(WritePlanImpl& plan, const Table& t, Expr* where)
{
    plan.table = &t;
    plan.sources.clear();
    SourcePlan sp;
    sp.table = &t;
    sp.alias = t.name;
    plan.sources.push_back(std::move(sp));
    bindColumns(where, plan.sources);
    std::vector<const Expr*> available;
    conjuncts(where, available);
    planAccess(plan.sources[0], available);
    plan.row.resize(t.width());
}

Result runUpdate(Catalog& cat, UpdateStmt& s, const Params& params)
{
    ResultBuilder out;
    bool fresh = false;
    WritePlanImpl& plan = writePlan(s.plan, cat, fresh);
    if (fresh) {
        const Table& t = cat.get(s.table);
        singleSource(plan, t, s.where.get());
        for (auto& [name, expr] : s.assignments) {
            const int slot = t.find(name);
            if (slot < 0)
                throw Error(ErrorCode::NoSuchColumn, "no such column: " + t.name + "." + name);
            plan.targets.push_back(slot);
            bindColumns(expr.get(), plan.sources);
        }
    }
    const Table& t = *plan.table;
    const std::vector<int>& targets = plan.targets;

    const std::vector<Rowbuf> matches =
        collectRows(cat, plan.sources[0], plan.row, s.where.get(), params);
    const nosql::Db& data = cat.rows(t);
    Rowbuf& after = plan.row;
    WriteScratch& ws = plan.ws;
    std::string& oldKey = plan.oldKey;
    Value tmp;

    for (const Rowbuf& before : matches) {
        for (std::size_t i = 0; i < after.size(); ++i)
            after[i] = before[i];
        for (std::size_t i = 0; i < targets.size(); ++i)
            after[std::size_t(targets[i])] =
                evaluate(*s.assignments[i].second, before, params, nullptr, tmp);
        finalizeRow(t, after);
        if (t.autoKey())
            cat.noteRowid(t, after[std::size_t(t.autoKeySlot())].integer());

        rowKeyInto(oldKey, t, before);
        rowKeyInto(ws.key, t, after);
        removeIndexEntries(cat, t, before, oldKey, ws);
        ws.payload.clear();
        encodeRowInto(ws.payload, after.data(), t.columns.size());
        if (ws.key == oldKey) {
            data.put(sliceOf(ws.key), sliceOf(ws.payload));
        } else {
            data.erase(sliceOf(oldKey));
            if (!data.put(sliceOf(ws.key), sliceOf(ws.payload), nosql::PutMode::InsertUnique))
                constraintFailed("PRIMARY KEY must be unique: " + t.name);
        }
        addIndexEntries(cat, t, after, ws.key, ws);
    }

    out.changed(matches.size());
    return out.result;
}

Result runDelete(Catalog& cat, DeleteStmt& s, const Params& params)
{
    ResultBuilder out;
    bool fresh = false;
    WritePlanImpl& plan = writePlan(s.plan, cat, fresh);
    if (fresh)
        singleSource(plan, cat.get(s.table), s.where.get());
    const Table& t = *plan.table;

    const std::vector<Rowbuf> matches =
        collectRows(cat, plan.sources[0], plan.row, s.where.get(), params);
    const nosql::Db& data = cat.rows(t);
    WriteScratch& ws = plan.ws;
    for (const Rowbuf& row : matches) {
        rowKeyInto(ws.key, t, row);
        removeIndexEntries(cat, t, row, ws.key, ws);
        data.erase(sliceOf(ws.key));
    }
    out.changed(matches.size());
    return out.result;
}

/// The nested loops of a SELECT: one level per source, the innermost handing
/// each complete row to `emit`.
template <class Emit>
struct Nest
{
    const Catalog& cat;
    std::vector<SourcePlan>& sources;
    Rowbuf& row;
    const Params& params;
    Emit& emit;

    bool operator()(std::size_t depth)
    {
        if (depth == sources.size())
            return emit();
        SourcePlan& sp = sources[depth];
        return scanSource(cat, sp, row, params, [&] {
            if (!holds(sp.on, row, params))
                return true;
            return (*this)(depth + 1);
        });
    }
};

/// Everything a SELECT derives from its text and the schema: bound column
/// slots (written into the tree), an access path per source, the projection,
/// the result's column names and, for FOR JSON, the property paths. Planned
/// once per schema snapshot; the buffers below are reused run after run.
struct SelectPlanImpl : SelectPlan
{
    std::shared_ptr<const Schema> schema;
    std::vector<SourcePlan> sources;
    std::size_t width = 0;
    std::vector<ExprPtr> synthetic;  ///< the columns a `*` expanded to
    std::vector<const Expr*> outputs;
    std::shared_ptr<const std::vector<std::string>> names;
    std::vector<std::string> jsonPaths;
    std::vector<Expr*> aggregates;
    bool grouped = false;
    std::int64_t wanted = -1;  ///< rows to produce before stopping, or -1
    // Scratch reused across runs.
    Rowbuf row;
    std::string groupKey;
    std::vector<Value> sortKeys;
    std::vector<std::uint32_t> order;
};

void planSelect(Catalog& cat, SelectStmt& s, SelectPlanImpl& plan, bool forJson)
{
    // --- sources, in join order ---
    std::vector<SourcePlan>& sources = plan.sources;
    sources.reserve(s.sources.size());
    std::size_t base = 0;
    for (SelectStmt::Source& src : s.sources) {
        const Table& t = cat.get(src.table);
        for (const SourcePlan& seen : sources) {
            if (equalsNoCase(seen.alias, src.alias))
                throw Error(ErrorCode::InvalidArgument, "duplicate table alias: " + src.alias);
        }
        SourcePlan sp;
        sp.table = &t;
        sp.alias = src.alias;
        sp.base = base;
        sp.on = src.on.get();
        base += t.width();
        sources.push_back(std::move(sp));
    }
    plan.width = base;

    // FOR JSON AUTO puts the first table at the top of the document and nests
    // each joined table one level further in, under its alias.
    const bool autoNesting = forJson && s.forJson->mode() == JsonMode::Auto;
    std::vector<std::string> autoPrefix(sources.size());
    for (std::size_t d = 1; d < sources.size(); ++d)
        autoPrefix[d] = autoPrefix[d - 1] + sources[d].alias + ".";

    /// The property a result column gets in the document. An unaliased column
    /// contributes its own name, not the `t.c` the result set labels it with,
    /// so that the table only ever shows up as a level of nesting.
    const auto jsonProperty = [](const SelectStmt::Item& item) -> const std::string& {
        if (!item.aliased && item.expr->kind == ExprKind::Column)
            return item.expr->column;
        return item.alias;
    };

    // --- the projection, with stars expanded ---
    std::vector<const Expr*>& outputs = plan.outputs;
    std::vector<std::string> names;
    std::vector<std::string>& jsonPaths = plan.jsonPaths;
    for (SelectStmt::Item& item : s.items) {
        if (!item.star) {
            bindColumns(item.expr.get(), sources);
            outputs.push_back(item.expr.get());
            names.push_back(item.alias);
            if (autoNesting)
                jsonPaths.push_back(
                    autoPrefix[std::size_t(deepestSource(item.expr.get(), sources))] +
                    jsonProperty(item));
            else if (forJson)
                jsonPaths.push_back(jsonProperty(item));
            continue;
        }
        bool matched = false;
        for (std::size_t d = 0; d < sources.size(); ++d) {
            const SourcePlan& sp = sources[d];
            if (!item.starTable.empty() && !equalsNoCase(item.starTable, sp.alias) &&
                !equalsNoCase(item.starTable, sp.table->name))
                continue;
            matched = true;
            for (std::size_t i = 0; i < sp.table->columns.size(); ++i) {
                auto column = std::make_unique<Expr>();
                column->kind = ExprKind::Column;
                column->column = sp.table->columns[i].name;
                column->slot = int(sp.base + i);
                column->columnType = sp.table->columns[i].type;
                outputs.push_back(column.get());
                names.push_back(sources.size() > 1 ? sp.alias + "." + column->column
                                                   : column->column);
                // A star across a join keeps the table level in both modes;
                // flattening it would collide on every shared column name.
                if (forJson)
                    jsonPaths.push_back(autoNesting ? autoPrefix[d] + column->column
                                                    : names.back());
                plan.synthetic.push_back(std::move(column));
            }
        }
        if (!matched)
            throw Error(ErrorCode::NoSuchTable, "no such table in FROM: " + item.starTable);
    }

    bindColumns(s.where.get(), sources);
    for (SelectStmt::Source& src : s.sources)
        bindColumns(src.on.get(), sources);
    for (ExprPtr& term : s.groupBy)
        bindColumns(term.get(), sources);
    bindColumns(s.having.get(), sources);
    for (SelectStmt::OrderTerm& term : s.order) {
        term.output = -1;
        // ORDER BY may name a result column, by alias or by position; that is
        // the only way to sort on something the row itself does not hold.
        if (term.expr->kind == ExprKind::Literal && term.expr->literal.type() == Type::Integer) {
            const std::int64_t n = term.expr->literal.integer();
            if (n < 1 || std::size_t(n) > outputs.size())
                throw Error(ErrorCode::InvalidArgument,
                            "ORDER BY " + std::to_string(n) + " is not a result column");
            term.output = int(n - 1);
            continue;
        }
        if (term.expr->kind == ExprKind::Column && term.expr->table.empty()) {
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (equalsNoCase(names[i], term.expr->column)) {
                    term.output = int(i);
                    break;
                }
            }
            if (term.output >= 0)
                continue;
        }
        bindColumns(term.expr.get(), sources);
    }

    // --- aggregates, numbered once so every group can use the same layout ---
    auto rejectAggregate = [](const Expr* e, const char* clause) {
        if (containsAggregate(e))
            throw Error(ErrorCode::InvalidArgument,
                        std::string("aggregate functions are not allowed in ") + clause);
    };
    rejectAggregate(s.where.get(), "WHERE");
    for (SelectStmt::Source& src : s.sources)
        rejectAggregate(src.on.get(), "ON");
    for (ExprPtr& term : s.groupBy)
        rejectAggregate(term.get(), "GROUP BY");

    std::vector<Expr*>& aggregates = plan.aggregates;
    for (SelectStmt::Item& item : s.items)
        collectAggregates(item.expr.get(), aggregates);
    collectAggregates(s.having.get(), aggregates);
    for (SelectStmt::OrderTerm& term : s.order) {
        if (term.output < 0)
            collectAggregates(term.expr.get(), aggregates);
    }
    plan.grouped = !s.groupBy.empty() || !aggregates.empty() || s.having != nullptr;

    // --- an access path per source, using everything bound to its left ---
    std::vector<const Expr*> available;
    conjuncts(s.where.get(), available);
    std::vector<const Expr*> here;
    for (std::size_t d = 0; d < sources.size(); ++d) {
        here = available;
        conjuncts(sources[d].on, here);
        planAccess(sources[d], here);
    }

    plan.wanted = (plan.grouped || s.limit < 0) ? -1 : s.limit + s.offset;
    plan.names = std::make_shared<const std::vector<std::string>>(std::move(names));
    plan.row.resize(plan.width);
}

/// Runs the SELECT and hands back its rows. `jsonPaths`, when asked for, comes
/// back pointing at the property path of every result column, which is how
/// FOR JSON AUTO nests by source table without disturbing the column names
/// that ORDER BY resolves against.
Result runSelectRows(Catalog& cat, SelectStmt& s, const Params& params,
                     const std::vector<std::string>** jsonPaths)
{
    const bool forJson = jsonPaths != nullptr && s.forJson.has_value();
    auto* plan = static_cast<SelectPlanImpl*>(s.plan.get());
    if (!plan || plan->schema != cat.schema()) {
        auto made = std::make_shared<SelectPlanImpl>();
        made->schema = cat.schema();
        planSelect(cat, s, *made, forJson);
        s.plan = made;
        plan = made.get();
    }
    if (jsonPaths)
        *jsonPaths = &plan->jsonPaths;

    std::vector<SourcePlan>& sources = plan->sources;
    const std::vector<const Expr*>& outputs = plan->outputs;
    const std::vector<Expr*>& aggregates = plan->aggregates;
    const bool grouped = plan->grouped;
    const bool sorted = !s.order.empty();
    const std::int64_t wanted = plan->wanted;

    ResultBuilder out;
    out.columns(plan->names);
    std::int64_t produced = 0;
    Rowbuf& row = plan->row;
    for (Value& v : row)
        v.setNull();

    // Rows kept for sorting: every projected row, and one flat run of sort
    // keys per row, ordered through an index so nothing is moved but ids.
    std::vector<std::vector<Value>> gathered;
    std::vector<Value>& sortKeys = plan->sortKeys;
    sortKeys.clear();
    const std::size_t orderWidth = s.order.size();

    /// One group's running state. `sample` is its first row, which is what a
    /// bare column outside GROUP BY reads from.
    struct Group
    {
        Rowbuf sample;
        std::vector<Accumulator> accumulators;
    };
    std::map<std::string, Group> groups;  ///< keyed by the encoded GROUP BY tuple
    std::string& groupKey = plan->groupKey;
    Value tmp;

    auto project = [&](const Rowbuf& r, Aggregates aggregateValues) {
        std::vector<Value> projected;
        projected.reserve(outputs.size());
        for (const Expr* e : outputs)
            projected.push_back(evaluate(*e, r, params, aggregateValues, tmp));
        return projected;
    };
    // Sort keys for one output row: a term naming a result column reads the
    // projection, anything else is evaluated against the row.
    auto gather = [&](std::vector<Value> projected, const Rowbuf& r, Aggregates aggregateValues) {
        for (const SelectStmt::OrderTerm& term : s.order)
            sortKeys.push_back(term.output >= 0
                                   ? projected[std::size_t(term.output)]
                                   : evaluate(*term.expr, r, params, aggregateValues, tmp));
        gathered.push_back(std::move(projected));
    };

    auto emit = [&]() -> bool {
        if (!holds(s.where.get(), row, params))
            return true;
        if (grouped) {
            groupKey.clear();
            for (const ExprPtr& term : s.groupBy)
                appendKey(groupKey, evaluate(*term, row, params, nullptr, tmp));
            auto it = groups.find(groupKey);
            if (it == groups.end())
                it = groups
                         .emplace(groupKey, Group{row, std::vector<Accumulator>(aggregates.size())})
                         .first;
            for (std::size_t i = 0; i < aggregates.size(); ++i)
                accumulate(it->second.accumulators[i], *aggregates[i], row, params);
            return true;
        }
        std::vector<Value> projected = project(row, nullptr);
        if (sorted) {
            gather(std::move(projected), row, nullptr);
        } else {
            if (produced >= s.offset)
                out.row(std::move(projected));
            ++produced;
            if (wanted >= 0 && produced >= wanted)
                return false;
        }
        return true;
    };
    Nest<decltype(emit)> nest{cat, sources, row, params, emit};
    nest(0);

    if (grouped) {
        // Aggregating nothing still yields one row -- COUNT(*) is 0, not absent
        // -- but only when there is no GROUP BY to produce a row per key.
        if (groups.empty() && s.groupBy.empty())
            groups.emplace(std::string(),
                           Group{Rowbuf(plan->width), std::vector<Accumulator>(aggregates.size())});

        for (auto& entry : groups) {
            const Group& group = entry.second;
            std::vector<Value> values(aggregates.size());
            for (std::size_t i = 0; i < aggregates.size(); ++i)
                values[i] = finish(group.accumulators[i], aggregates[i]->aggregate);
            if (!holds(s.having.get(), group.sample, params, &values))
                continue;
            gather(project(group.sample, &values), group.sample, &values);
        }
    } else if (!sorted) {
        return out.result;  // already emitted, LIMIT and OFFSET included
    }

    std::vector<std::uint32_t>& order = plan->order;
    order.resize(gathered.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = std::uint32_t(i);
    if (sorted) {
        std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
            const Value* ka = sortKeys.data() + std::size_t(a) * orderWidth;
            const Value* kb = sortKeys.data() + std::size_t(b) * orderWidth;
            for (std::size_t i = 0; i < orderWidth; ++i) {
                const int c = compare(ka[i], kb[i]);
                if (c)
                    return s.order[i].descending ? c > 0 : c < 0;
            }
            return false;
        });
    }

    const std::size_t from = std::min<std::size_t>(std::size_t(s.offset), gathered.size());
    std::size_t to = gathered.size();
    if (s.limit >= 0)
        to = std::min<std::size_t>(to, from + std::size_t(s.limit));
    for (std::size_t i = from; i < to; ++i)
        out.row(std::move(gathered[order[i]]));
    return out.result;
}

Result runSelect(Catalog& cat, SelectStmt& s, const Params& params)
{
    if (!s.forJson)
        return runSelectRows(cat, s, params, nullptr);

    const std::vector<std::string>* paths = nullptr;
    const Result rows = runSelectRows(cat, s, params, &paths);

    // One writer per thread, kept alive between statements: the document
    // buffer and the plan it derives from the column names are the two things
    // a long-running process would otherwise reallocate on every query.
    static thread_local JsonWriter writer;
    writer.options(*s.forJson);

    ResultBuilder out;
    static const auto kJsonColumn =
        std::make_shared<const std::vector<std::string>>(1, std::string("json"));
    out.columns(kJsonColumn);
    std::string document;
    writer.appendTo(rows, *paths, document);
    out.row({Value(std::move(document))});
    return out.result;
}

}  // namespace

Result execute(Catalog& catalog, Statement& stmt, const Params& params)
{
    const auto run = [&]() -> Result {
        if (auto* create = std::get_if<CreateTableStmt>(&stmt))
            return runCreateTable(catalog, *create);
        if (auto* drop = std::get_if<DropTableStmt>(&stmt))
            return runDropTable(catalog, *drop);
        if (auto* createIndex = std::get_if<CreateIndexStmt>(&stmt))
            return runCreateIndex(catalog, *createIndex);
        if (auto* dropIndex = std::get_if<DropIndexStmt>(&stmt))
            return runDropIndex(catalog, *dropIndex);
        if (auto* insert = std::get_if<InsertStmt>(&stmt))
            return runInsert(catalog, *insert, params);
        if (auto* update = std::get_if<UpdateStmt>(&stmt))
            return runUpdate(catalog, *update, params);
        if (auto* remove = std::get_if<DeleteStmt>(&stmt))
            return runDelete(catalog, *remove, params);
        return runSelect(catalog, std::get<SelectStmt>(stmt), params);
    };
    Result result = run();
    // Whatever the statement left pending -- a rowid counter -- reaches the
    // store before the next statement or the commit sees it.
    catalog.flush();
    return result;
}

}  // namespace sql::internal
