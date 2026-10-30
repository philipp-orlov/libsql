// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Expression evaluation: one switch over the annotated tree, values read from
// the frame or computed into each node's own scratch cell.

#include <cmath>
#include <limits>

#include "internal/encoding.hpp"
#include "internal/exec.hpp"
#include "internal/raise.hpp"

namespace sql::internal {

namespace {

inline const Value& nullValue()
{
    static const Value kNull;
    return kNull;
}

bool isNumericAffinity(Affinity a) noexcept
{
    return a == Affinity::Integer || a == Affinity::Real || a == Affinity::Numeric;
}

void setTruth(Value& out, bool v)
{
    out.setInteger(v ? 1 : 0);
}

void arithmetic(BinaryOp op, const Value& left, const Value& right, Value& out)
{
    Value a, b;
    numericOperand(left, a);
    numericOperand(right, b);
    if (a.type() == Type::Integer && b.type() == Type::Integer) {
        const std::int64_t x = a.integer(), y = b.integer();
        std::int64_t r = 0;
        switch (op) {
            case BinaryOp::Add:
                if (!__builtin_add_overflow(x, y, &r)) {
                    out.setInteger(r);
                    return;
                }
                out.setReal(double(x) + double(y));
                return;
            case BinaryOp::Sub:
                if (!__builtin_sub_overflow(x, y, &r)) {
                    out.setInteger(r);
                    return;
                }
                out.setReal(double(x) - double(y));
                return;
            case BinaryOp::Mul:
                if (!__builtin_mul_overflow(x, y, &r)) {
                    out.setInteger(r);
                    return;
                }
                out.setReal(double(x) * double(y));
                return;
            case BinaryOp::Div:
                if (y == 0) {
                    out.setNull();
                    return;
                }
                if (x == std::numeric_limits<std::int64_t>::min() && y == -1) {
                    out.setReal(-double(x));
                    return;
                }
                out.setInteger(x / y);
                return;
            default:
                if (y == 0) {
                    out.setNull();
                    return;
                }
                out.setInteger(y == -1 ? 0 : x % y);
                return;
        }
    }
    const double x = a.type() == Type::Real ? a.real() : double(a.integer());
    const double y = b.type() == Type::Real ? b.real() : double(b.integer());
    double r = 0;
    switch (op) {
        case BinaryOp::Add: r = x + y; break;
        case BinaryOp::Sub: r = x - y; break;
        case BinaryOp::Mul: r = x * y; break;
        case BinaryOp::Div:
            if (y == 0) {
                out.setNull();
                return;
            }
            r = x / y;
            break;
        default: {
            const std::int64_t ix = toInt64(a), iy = toInt64(b);
            if (iy == 0) {
                out.setNull();
                return;
            }
            r = double(iy == -1 ? 0 : ix % iy);
            break;
        }
    }
    if (std::isnan(r))
        out.setNull();
    else
        out.setReal(r);
}

std::int64_t intOperand(const Value& v)
{
    Value n;
    numericOperand(v, n);
    return n.type() == Type::Integer ? n.integer() : toInt64(n);
}

void bitwise(BinaryOp op, const Value& left, const Value& right, Value& out)
{
    const std::int64_t x = intOperand(left);
    std::int64_t y = intOperand(right);
    switch (op) {
        case BinaryOp::BitAnd: out.setInteger(x & y); return;
        case BinaryOp::BitOr: out.setInteger(x | y); return;
        default: break;
    }
    bool left_ = op == BinaryOp::ShiftLeft;
    if (y < 0) {
        left_ = !left_;
        y = y <= -64 ? 64 : -y;
    }
    if (y >= 64) {
        out.setInteger(!left_ && x < 0 ? -1 : 0);
        return;
    }
    if (left_)
        out.setInteger(std::int64_t(std::uint64_t(x) << y));
    else
        out.setInteger(x >> y);
}

bool isDatetimeOperand(const Expr& e) noexcept
{
    return e.strictDatetime;
}

/// libsql's DATETIME columns compare text and integers as instants.
bool datetimeAffinity(Value& v)
{
    if (v.type() == Type::Text) {
        try {
            v.setDatetime(parseDatetime(v.text()));
            return true;
        } catch (const Error&) {
            return false;
        }
    }
    return false;
}

}  // namespace

Affinity comparisonAffinity(const Expr& self, const Expr& other) noexcept
{
    const Affinity a = self.hasAffinity ? self.affinity : Affinity::Blob;
    const Affinity b = other.hasAffinity ? other.affinity : Affinity::Blob;
    if (isNumericAffinity(b) && !isNumericAffinity(a))
        return Affinity::Numeric;
    if (b == Affinity::Text && a == Affinity::Blob)
        return Affinity::Text;
    return Affinity::Blob;
}

Collation comparisonCollation(const Expr& l, const Expr& r) noexcept
{
    if (l.collationStrength == 2)
        return l.collation;
    if (r.collationStrength == 2)
        return r.collation;
    if (l.collationStrength == 1)
        return l.collation;
    if (r.collationStrength == 1)
        return r.collation;
    return Collation::Binary;
}

int compareOperands(const Expr& l, const Value& a, const Expr& r, const Value& b, Collation coll,
                    bool& isNull)
{
    if (a.isNull() || b.isNull()) {
        isNull = true;
        return 0;
    }
    isNull = false;
    const Affinity la = comparisonAffinity(l, r), ra = comparisonAffinity(r, l);
    const bool dt = (isDatetimeOperand(l) && b.type() == Type::Text) ||
                    (isDatetimeOperand(r) && a.type() == Type::Text);
    // Affinity only ever changes text into a number or a number into text;
    // copy an operand only when that is about to happen.
    auto converts = [](const Value& v, Affinity aff) {
        if (aff == Affinity::Blob)
            return false;
        if (aff == Affinity::Text)
            return v.type() == Type::Integer || v.type() == Type::Real;
        return v.type() == Type::Text;
    };
    if (!dt && !converts(a, la) && !converts(b, ra))
        return compareValues(a, b, coll);
    Value x = a, y = b;
    if (la != Affinity::Blob)
        applyComparisonAffinity(x, la);
    if (ra != Affinity::Blob)
        applyComparisonAffinity(y, ra);
    if (dt) {
        if (isDatetimeOperand(l))
            datetimeAffinity(y);
        if (isDatetimeOperand(r))
            datetimeAffinity(x);
    }
    return compareValues(x, y, coll);
}

namespace {

// ------------------------------------------------- direct comparisons ----
//
// A comparison of a column with a constant (or two columns) is most of what
// WHERE and ON clauses hold. compareOperands copies and converts only when
// affinity would change an operand; when neither value changes it is plain
// compareValues. Which value types change depends only on the two operand
// expressions, so it is settled once per node here, and the common cases --
// two numbers, or two texts under BINARY -- are then compared without
// evaluating the operand nodes or consulting affinity again.

enum : std::uint8_t
{
    kShapeNone = 1,
    kShapeDirect = 2,     ///< both operands are columns, literals or parameters
    kNumericOk = 1 << 2,  ///< two numbers compare as they are
    kTextOk = 1 << 3,     ///< two texts compare as they are (BINARY)
};

bool directOperand(const Expr& x) noexcept
{
    return x.kind == ExprKind::Column || x.kind == ExprKind::Literal || x.kind == ExprKind::Parameter;
}

std::uint8_t classifyComparison(const Expr& e) noexcept
{
    const Expr& l = *e.lhs;
    const Expr& r = *e.rhs;
    if (!directOperand(l) || !directOperand(r) || l.sub || r.sub)
        return kShapeNone;
    const Affinity la = comparisonAffinity(l, r), ra = comparisonAffinity(r, l);
    auto numeric = [](Affinity a) { return a == Affinity::Integer || a == Affinity::Real || a == Affinity::Numeric; };
    std::uint8_t shape = kShapeDirect;
    if (la != Affinity::Text && ra != Affinity::Text)
        shape |= kNumericOk;
    if (!numeric(la) && !numeric(ra) && !isDatetimeOperand(l) && !isDatetimeOperand(r) &&
        e.collation == Collation::Binary)
        shape |= kTextOk;
    return shape;
}

inline const Value& directValue(const Expr& x, const Ctx& c) noexcept
{
    if (x.kind == ExprKind::Column)
        return c.slots[x.slot];
    if (x.kind == ExprKind::Literal)
        return x.literal;
    if (!c.params || x.param >= c.params->size())
        return nullValue();
    return (*c.params)[x.param];
}

}  // namespace

/// 1 true, 0 false, -1 NULL, 2 not decided here (use the general path).
int directCompare(const Expr& e, const Ctx& c) noexcept
{
    if (e.fastShape == 0)
        e.fastShape = classifyComparison(e);
    const std::uint8_t shape = e.fastShape;
    if (!(shape & kShapeDirect))
        return 2;
    const Value& a = directValue(*e.lhs, c);
    const Value& b = directValue(*e.rhs, c);
    const Type ta = a.type(), tb = b.type();
    if (ta == Type::Null || tb == Type::Null)
        return -1;
    int cmp;
    if (ta == Type::Integer && tb == Type::Integer && (shape & kNumericOk)) {
        cmp = a.integer() < b.integer() ? -1 : a.integer() > b.integer();
    } else if (ta == Type::Real && tb == Type::Real && (shape & kNumericOk)) {
        cmp = a.real() < b.real() ? -1 : a.real() > b.real();
    } else if (ta == Type::Integer && tb == Type::Real && (shape & kNumericOk)) {
        cmp = compareIntReal(a.integer(), b.real());
    } else if (ta == Type::Real && tb == Type::Integer && (shape & kNumericOk)) {
        cmp = -compareIntReal(b.integer(), a.real());
    } else if (ta == Type::Text && tb == Type::Text && (shape & kTextOk)) {
        cmp = a.text().compare(b.text());
    } else {
        return 2;
    }
    switch (BinaryOp(e.op)) {
        case BinaryOp::Eq: return cmp == 0;
        case BinaryOp::Ne: return cmp != 0;
        case BinaryOp::Lt: return cmp < 0;
        case BinaryOp::Le: return cmp <= 0;
        case BinaryOp::Gt: return cmp > 0;
        case BinaryOp::Ge: return cmp >= 0;
        default: return 2;
    }
}

bool comparesNumbersDirectly(const Expr& e) noexcept
{
    if (e.fastShape == 0)
        e.fastShape = classifyComparison(e);
    return (e.fastShape & (kShapeDirect | kNumericOk)) == (kShapeDirect | kNumericOk);
}

namespace {

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

/// The parts of a row value operand: a RowValue's elements, or the columns
/// of a sub-query's first row.
struct RowOperand
{
    std::vector<const Value*> values;
    std::vector<const Expr*> exprs;
};

void rowOperand(const Expr& e, Ctx& c, RowOperand& out)
{
    out.values.clear();
    out.exprs.clear();
    if (e.kind == ExprKind::RowValue) {
        for (const ExprPtr& x : e.list) {
            out.values.push_back(&eval(*x, c));
            out.exprs.push_back(x.get());
        }
        return;
    }
    if (e.kind == ExprKind::Subquery && e.sub && e.sub->width > 1) {
        runSubquery(*e.sub, c);
        for (std::size_t i = 0; i < e.sub->width; ++i) {
            out.values.push_back(e.sub->rows.empty() ? &nullValue() : &e.sub->rows.row(0)[i]);
            out.exprs.push_back(&e);
        }
        return;
    }
    out.values.push_back(&eval(e, c));
    out.exprs.push_back(&e);
}

/// Row-value comparison, three-valued: returns 1, 0, or -1 for NULL.
int compareRows(BinaryOp op, const Expr& l, const Expr& r, Ctx& c, bool nullSafe)
{
    RowOperand a, b;
    rowOperand(l, c, a);
    rowOperand(r, c, b);
    if (a.values.size() != b.values.size())
        throw Error(ErrorCode::Runtime, "row value misused");
    const bool equality = op == BinaryOp::Eq || op == BinaryOp::Ne;
    bool sawNull = false;
    for (std::size_t i = 0; i < a.values.size(); ++i) {
        const Value& x = *a.values[i];
        const Value& y = *b.values[i];
        if (x.isNull() || y.isNull()) {
            if (nullSafe) {
                if (x.isNull() != y.isNull())
                    return op == BinaryOp::Ne ? 1 : 0;
                continue;
            }
            if (!equality)
                return -1;
            sawNull = true;
            continue;
        }
        bool isNull = false;
        const int cmp = compareOperands(*a.exprs[i], x, *b.exprs[i], y,
                                        comparisonCollation(*a.exprs[i], *b.exprs[i]), isNull);
        if (cmp != 0) {
            if (equality)
                return op == BinaryOp::Ne ? 1 : 0;
            return relation(op, cmp) ? 1 : 0;
        }
    }
    if (sawNull)
        return -1;
    return relation(op, 0) ? 1 : 0;
}

void concat(const Value& a, const Value& b, Value& out)
{
    std::string s;
    appendText(a, s);
    appendText(b, s);
    out.setText(s);
}

/// LIKE's escape character, validated.
std::uint32_t escapeOf(const Expr& e, Ctx& c, bool& isNull)
{
    if (!e.extra)
        return 0;
    const Value& v = eval(*e.extra, c);
    if (v.isNull()) {
        isNull = true;
        return 0;
    }
    std::string scratch;
    const std::string_view s = textOf(v, scratch);
    std::size_t i = 0;
    if (s.empty())
        throw Error(ErrorCode::Runtime, "ESCAPE expression must be a single character");
    const std::uint32_t cp = utf8Next(s, i);
    if (i != s.size())
        throw Error(ErrorCode::Runtime, "ESCAPE expression must be a single character");
    return cp;
}

const Value& evalLike(const Expr& e, Ctx& c)
{
    const LikeKind kind = LikeKind(e.op);
    if (kind == LikeKind::Regexp || kind == LikeKind::Match) {
        // X REGEXP Y is regexp(Y, X); the planner resolved the function.
        const Value& x = eval(*e.lhs, c);
        const Value& y = eval(*e.rhs, c);
        const Value* argv[2] = {&y, &x};
        e.fn->scalar(*c.fctx, 2, argv, e.scratch);
        if (e.negated && !e.scratch.isNull())
            setTruth(e.scratch, !isTrue(e.scratch));
        return e.scratch;
    }
    const Value& x = eval(*e.lhs, c);
    const Value& p = eval(*e.rhs, c);
    bool escNull = false;
    const std::uint32_t esc = escapeOf(e, c, escNull);
    if (x.isNull() || p.isNull() || escNull) {
        e.scratch.setNull();
        return e.scratch;
    }
    // Both sides are C strings to SQLite's matcher: they end at a NUL.
    auto cstr = [](std::string_view v) {
        const std::size_t z = v.find('\0');
        return z == std::string_view::npos ? v : v.substr(0, z);
    };
    std::string sx, sp;
    const std::string_view tx = cstr(textOf(x, sx));
    const std::string_view tp = cstr(textOf(p, sp));
    if (tp.size() > 50000)
        throw Error(ErrorCode::Runtime, "LIKE or GLOB pattern too complex");
    bool m;
    if (kind == LikeKind::Glob)
        m = globMatch(tp, tx);
    else
        m = likeMatch(tp, tx, esc, c.session && c.session->conn && c.session->conn->caseSensitiveLike);
    setTruth(e.scratch, m != e.negated);
    return e.scratch;
}

const Value& evalInList(const Expr& e, Ctx& c)
{
    const Value& x = eval(*e.lhs, c);
    if (e.list.empty()) {
        setTruth(e.scratch, e.negated);
        return e.scratch;
    }
    if (x.isNull()) {
        e.scratch.setNull();
        return e.scratch;
    }
    const Collation coll = e.lhs->collationStrength ? e.lhs->collation : Collation::Binary;
    bool sawNull = false;
    for (const ExprPtr& item : e.list) {
        const Value& v = eval(*item, c);
        if (v.isNull()) {
            sawNull = true;
            continue;
        }
        bool isNull = false;
        if (compareOperands(*e.lhs, x, *item, v, comparisonCollation(*e.lhs, *item), isNull) == 0) {
            (void)coll;
            setTruth(e.scratch, !e.negated);
            return e.scratch;
        }
    }
    if (sawNull)
        e.scratch.setNull();
    else
        setTruth(e.scratch, e.negated);
    return e.scratch;
}

const Value& evalInSelect(const Expr& e, Ctx& c)
{
    SubqueryPlan& s = *e.sub;
    RowOperand lhs;
    rowOperand(*e.lhs, c, lhs);
    runSubquery(s, c);
    if (s.keys.empty() && !s.hasNull) {
        setTruth(e.scratch, e.negated);
        return e.scratch;
    }
    std::string& key = c.fctx->buf2;
    key.clear();
    bool lhsNull = false;
    for (std::size_t i = 0; i < lhs.values.size(); ++i) {
        const Value& v = *lhs.values[i];
        if (v.isNull()) {
            lhsNull = true;
            break;
        }
        if (s.keyAffinity[i] != Affinity::Blob ||
            (lhs.exprs[i]->strictDatetime && v.type() == Type::Text)) {
            Value conv = v;
            applyComparisonAffinity(conv, s.keyAffinity[i]);
            appendKey(key, conv, s.keyCollation[i]);
        } else {
            appendKey(key, v, s.keyCollation[i]);
        }
    }
    if (lhsNull) {
        e.scratch.setNull();
        return e.scratch;
    }
    if (s.keys.contains(key)) {
        setTruth(e.scratch, !e.negated);
        return e.scratch;
    }
    if (s.hasNull)
        e.scratch.setNull();
    else
        setTruth(e.scratch, e.negated);
    return e.scratch;
}

const Value& evalBetween(const Expr& e, Ctx& c)
{
    const Value& x = eval(*e.lhs, c);
    const Value& lo = eval(*e.list[0], c);
    const Value& hi = eval(*e.list[1], c);
    bool n1 = false, n2 = false;
    const int a = compareOperands(*e.lhs, x, *e.list[0], lo, comparisonCollation(*e.lhs, *e.list[0]), n1);
    const int b = compareOperands(*e.lhs, x, *e.list[1], hi, comparisonCollation(*e.lhs, *e.list[1]), n2);
    // x >= lo AND x <= hi, three-valued.
    const int ge = n1 ? -1 : (a >= 0 ? 1 : 0);
    const int le = n2 ? -1 : (b <= 0 ? 1 : 0);
    int r;
    if (ge == 0 || le == 0)
        r = 0;
    else if (ge < 0 || le < 0)
        r = -1;
    else
        r = 1;
    if (r < 0)
        e.scratch.setNull();
    else
        setTruth(e.scratch, (r == 1) != e.negated);
    return e.scratch;
}

const Value& evalCase(const Expr& e, Ctx& c)
{
    if (e.lhs) {
        const Value& x = eval(*e.lhs, c);
        for (std::size_t i = 0; i + 1 < e.list.size(); i += 2) {
            const Value& w = eval(*e.list[i], c);
            bool isNull = false;
            if (!x.isNull() &&
                compareOperands(*e.lhs, x, *e.list[i], w, comparisonCollation(*e.lhs, *e.list[i]),
                                isNull) == 0 &&
                !isNull)
                return eval(*e.list[i + 1], c);
        }
    } else {
        for (std::size_t i = 0; i + 1 < e.list.size(); i += 2) {
            if (isTrue(eval(*e.list[i], c)))
                return eval(*e.list[i + 1], c);
        }
    }
    if (e.rhs)
        return eval(*e.rhs, c);
    return nullValue();
}

const Value& evalFunction(const Expr& e, Ctx& c)
{
    switch (e.op) {
        case kCoalesce:
            for (const ExprPtr& a : e.list) {
                const Value& v = eval(*a, c);
                if (!v.isNull())
                    return v;
            }
            return nullValue();
        case kIif:
            return isTrue(eval(*e.list[0], c)) ? eval(*e.list[1], c) : eval(*e.list[2], c);
        case kIdentity: return eval(*e.list[0], c);
        default: break;
    }
    const std::size_t n = e.list.size();
    if (e.argv.size() != n)
        e.argv.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        e.argv[i] = &eval(*e.list[i], c);
    c.fctx->collation = e.collation;
    e.fn->scalar(*c.fctx, int(n), e.argv.data(), e.scratch);
    return e.scratch;
}

}  // namespace

const Value& eval(const Expr& e, Ctx& c)
{
    switch (e.kind) {
        case ExprKind::Literal: return e.literal;
        case ExprKind::Column:
        case ExprKind::Slot:
        case ExprKind::Aggregate:
        case ExprKind::Window: return c.slots[e.slot];
        case ExprKind::Parameter:
            if (!c.params || e.param >= c.params->size())
                return nullValue();
            return (*c.params)[e.param];

        case ExprKind::Unary: {
            const Value& v = eval(*e.lhs, c);
            if (v.isNull())
                return nullValue();
            switch (UnaryOp(e.op)) {
                case UnaryOp::Not: setTruth(e.scratch, !isTrue(v)); return e.scratch;
                case UnaryOp::Plus: return v;
                case UnaryOp::BitNot: e.scratch.setInteger(~intOperand(v)); return e.scratch;
                case UnaryOp::Negate: {
                    Value n;
                    numericOperand(v, n);
                    if (n.type() == Type::Integer) {
                        if (n.integer() == std::numeric_limits<std::int64_t>::min())
                            e.scratch.setReal(9223372036854775808.0);
                        else
                            e.scratch.setInteger(-n.integer());
                    } else {
                        e.scratch.setReal(-n.real());
                    }
                    return e.scratch;
                }
            }
            return nullValue();
        }

        case ExprKind::Binary: {
            const BinaryOp op = BinaryOp(e.op);
            switch (op) {
                case BinaryOp::And: {
                    const Value& a = eval(*e.lhs, c);
                    const bool an = a.isNull();
                    if (!an && !isTrue(a)) {
                        setTruth(e.scratch, false);
                        return e.scratch;
                    }
                    const Value& b = eval(*e.rhs, c);
                    if (!b.isNull() && !isTrue(b)) {
                        setTruth(e.scratch, false);
                        return e.scratch;
                    }
                    if (an || b.isNull())
                        return nullValue();
                    setTruth(e.scratch, true);
                    return e.scratch;
                }
                case BinaryOp::Or: {
                    const Value& a = eval(*e.lhs, c);
                    const bool an = a.isNull();
                    if (!an && isTrue(a)) {
                        setTruth(e.scratch, true);
                        return e.scratch;
                    }
                    const Value& b = eval(*e.rhs, c);
                    if (!b.isNull() && isTrue(b)) {
                        setTruth(e.scratch, true);
                        return e.scratch;
                    }
                    if (an || b.isNull())
                        return nullValue();
                    setTruth(e.scratch, false);
                    return e.scratch;
                }
                case BinaryOp::Eq:
                case BinaryOp::Ne:
                case BinaryOp::Lt:
                case BinaryOp::Le:
                case BinaryOp::Gt:
                case BinaryOp::Ge: {
                    if (e.lhs->kind == ExprKind::RowValue || e.rhs->kind == ExprKind::RowValue ||
                        (e.lhs->sub && e.lhs->sub->width > 1) || (e.rhs->sub && e.rhs->sub->width > 1)) {
                        const int r = compareRows(op, *e.lhs, *e.rhs, c, false);
                        if (r < 0)
                            return nullValue();
                        setTruth(e.scratch, r == 1);
                        return e.scratch;
                    }
                    if (const int d = directCompare(e, c); d != 2) {
                        if (d < 0)
                            return nullValue();
                        setTruth(e.scratch, d == 1);
                        return e.scratch;
                    }
                    const Value& a = eval(*e.lhs, c);
                    const Value& b = eval(*e.rhs, c);
                    bool isNull = false;
                    const int cmp = compareOperands(*e.lhs, a, *e.rhs, b, e.collation, isNull);
                    if (isNull)
                        return nullValue();
                    setTruth(e.scratch, relation(op, cmp));
                    return e.scratch;
                }
                case BinaryOp::Concat: {
                    const Value& a = eval(*e.lhs, c);
                    if (a.isNull())
                        return nullValue();
                    const Value& b = eval(*e.rhs, c);
                    if (b.isNull())
                        return nullValue();
                    concat(a, b, e.scratch);
                    return e.scratch;
                }
                case BinaryOp::BitAnd:
                case BinaryOp::BitOr:
                case BinaryOp::ShiftLeft:
                case BinaryOp::ShiftRight: {
                    const Value& a = eval(*e.lhs, c);
                    const Value& b = eval(*e.rhs, c);
                    if (a.isNull() || b.isNull())
                        return nullValue();
                    bitwise(op, a, b, e.scratch);
                    return e.scratch;
                }
                default: {
                    const Value& a = eval(*e.lhs, c);
                    const Value& b = eval(*e.rhs, c);
                    if (a.isNull() || b.isNull())
                        return nullValue();
                    arithmetic(op, a, b, e.scratch);
                    return e.scratch;
                }
            }
        }

        case ExprKind::Is: {
            if (e.lhs->kind == ExprKind::RowValue || e.rhs->kind == ExprKind::RowValue) {
                const int r = compareRows(e.negated ? BinaryOp::Ne : BinaryOp::Eq, *e.lhs, *e.rhs,
                                          c, true);
                setTruth(e.scratch, r == 1);
                return e.scratch;
            }
            const Value& a = eval(*e.lhs, c);
            const Value& b = eval(*e.rhs, c);
            bool equal;
            if (a.isNull() || b.isNull()) {
                equal = a.isNull() && b.isNull();
            } else {
                bool isNull = false;
                equal = compareOperands(*e.lhs, a, *e.rhs, b, e.collation, isNull) == 0;
            }
            setTruth(e.scratch, equal != e.negated);
            return e.scratch;
        }

        case ExprKind::InList: return evalInList(e, c);
        case ExprKind::InSelect: return evalInSelect(e, c);
        case ExprKind::Between: return evalBetween(e, c);
        case ExprKind::Like: return evalLike(e, c);

        case ExprKind::Cast: {
            const Value& v = eval(*e.lhs, c);
            e.scratch = v;
            e.scratch.setSubtype(0);
            castTo(e.scratch, e.affinity);
            return e.scratch;
        }

        case ExprKind::Case: return evalCase(e, c);
        case ExprKind::Collate: return eval(*e.lhs, c);
        case ExprKind::Function: return evalFunction(e, c);

        case ExprKind::Subquery: {
            runSubquery(*e.sub, c);
            if (e.sub->width > 1)
                throw Error(ErrorCode::Runtime, "row value misused");
            return e.sub->scalar;
        }
        case ExprKind::Exists:
            runSubquery(*e.sub, c);
            setTruth(e.scratch, e.sub->exists != e.negated);
            return e.scratch;

        case ExprKind::RowValue: throw Error(ErrorCode::Runtime, "row value misused");

        case ExprKind::Raise: {
            const RaiseKind k = RaiseKind(e.op);
            if (k == RaiseKind::Ignore)
                throw TriggerIgnore();
            throw RaiseError(k, e.literal.isNull() ? std::string() : std::string(e.literal.text()));
        }
    }
    return nullValue();
}

bool holdsEval(const Expr& e, Ctx& c)
{
    return isTrue(eval(e, c));
}

}  // namespace sql::internal
