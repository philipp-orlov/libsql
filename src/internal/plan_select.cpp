// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Name resolution and SELECT planning.

#include <algorithm>
#include <set>

#include "internal/parser.hpp"
#include "internal/plan.hpp"
#include "internal/util.hpp"

namespace sql::internal {

void presentAsJson(SelectPlan& sp, const JsonOptions& options);

namespace {

/// Problems found while planning: the statement cannot run as written.
[[noreturn]] void error(const std::string& what)
{
    throw Error(ErrorCode::InvalidArgument, what);
}

[[noreturn]] void noColumn(const std::string& what)
{
    throw Error(ErrorCode::NoSuchColumn, "no such column: " + what);
}

bool isWindowBuiltin(std::string_view name, std::uint8_t& code)
{
    static const char* kNames[] = {"row_number",   "rank", "dense_rank", "percent_rank",
                                   "cume_dist",    "ntile", "lag",        "lead",
                                   "first_value",  "last_value", "nth_value"};
    for (std::uint8_t i = 0; i < std::size(kNames); ++i) {
        if (equalsNoCase(name, kNames[i])) {
            code = std::uint8_t(i + 1);
            return true;
        }
    }
    return false;
}

void windowArity(std::uint8_t code, std::size_t argc, const std::string& name)
{
    std::size_t lo = 0, hi = 0;
    switch (code) {
        case 1:
        case 2:
        case 3:
        case 4:
        case 5: lo = hi = 0; break;
        case 6: lo = hi = 1; break;
        case 7:
        case 8: lo = 1; hi = 3; break;
        case 9:
        case 10: lo = hi = 1; break;
        case 11: lo = hi = 2; break;
        default: break;
    }
    if (argc < lo || argc > hi)
        error("wrong number of arguments to function " + name + "()");
}

/// Walks every Column/Aggregate/Window/Slot slot an expression reads.
template <class Fn>
void forEachSlot(const Expr* e, Fn&& fn)
{
    if (!e)
        return;
    switch (e->kind) {
        case ExprKind::Column:
        case ExprKind::Slot:
        case ExprKind::Aggregate:
        case ExprKind::Window: fn(e->slot); break;
        default: break;
    }
    if (e->kind == ExprKind::Aggregate || e->kind == ExprKind::Window)
        return;
    forEachSlot(e->lhs.get(), fn);
    forEachSlot(e->rhs.get(), fn);
    forEachSlot(e->extra.get(), fn);
    for (const ExprPtr& x : e->list)
        forEachSlot(x.get(), fn);
}

bool containsKind(const Expr* e, ExprKind k)
{
    if (!e)
        return false;
    if (e->kind == k)
        return true;
    if (containsKind(e->lhs.get(), k) || containsKind(e->rhs.get(), k) ||
        containsKind(e->extra.get(), k) || containsKind(e->filter.get(), k))
        return true;
    for (const ExprPtr& x : e->list) {
        if (containsKind(x.get(), k))
            return true;
    }
    return false;
}

/// Copies parameter numbers from `from` onto the identical shape `to` (a
/// re-parse of the same text numbers its `?` from one again).
void copyParams(const Expr& from, Expr& to)
{
    if (from.kind == ExprKind::Parameter && to.kind == ExprKind::Parameter)
        to.param = from.param;
    if (from.lhs && to.lhs)
        copyParams(*from.lhs, *to.lhs);
    if (from.rhs && to.rhs)
        copyParams(*from.rhs, *to.rhs);
    if (from.extra && to.extra)
        copyParams(*from.extra, *to.extra);
    if (from.filter && to.filter)
        copyParams(*from.filter, *to.filter);
    for (std::size_t i = 0; i < from.list.size() && i < to.list.size(); ++i)
        copyParams(*from.list[i], *to.list[i]);
}

std::string ordinal(std::size_t n)
{
    const char* suffix = "th";
    if (n % 100 < 11 || n % 100 > 13) {
        if (n % 10 == 1)
            suffix = "st";
        else if (n % 10 == 2)
            suffix = "nd";
        else if (n % 10 == 3)
            suffix = "rd";
    }
    return std::to_string(n) + suffix;
}

}  // namespace

bool sameExpr(const Expr& a, const Expr& b) noexcept
{
    if (a.kind != b.kind || a.op != b.op || a.negated != b.negated || a.distinct != b.distinct ||
        a.star != b.star)
        return false;
    switch (a.kind) {
        case ExprKind::Literal:
            if (a.literal.type() != b.literal.type() || compareValues(a.literal, b.literal) != 0)
                return false;
            break;
        case ExprKind::Column:
        case ExprKind::Slot:
            if (a.slot != b.slot)
                return false;
            break;
        case ExprKind::Parameter:
            if (a.param != b.param)
                return false;
            break;
        case ExprKind::Function:
        case ExprKind::Aggregate:
        case ExprKind::Collate:
        case ExprKind::Cast:
            if (!equalsNoCase(a.name, b.name))
                return false;
            break;
        case ExprKind::Subquery:
        case ExprKind::Exists:
        case ExprKind::InSelect:
        case ExprKind::Window:
        case ExprKind::Raise: return &a == &b;
        default: break;
    }
    auto same = [](const ExprPtr& x, const ExprPtr& y) {
        if (!x || !y)
            return !x && !y;
        return sameExpr(*x, *y);
    };
    if (!same(a.lhs, b.lhs) || !same(a.rhs, b.rhs) || !same(a.extra, b.extra))
        return false;
    if (a.list.size() != b.list.size())
        return false;
    for (std::size_t i = 0; i < a.list.size(); ++i) {
        if (!sameExpr(*a.list[i], *b.list[i]))
            return false;
    }
    return true;
}

// ----------------------------------------------------------- utilities ---

std::size_t Planner::allocate(std::size_t n)
{
    const std::size_t at = prog_.frameSize;
    prog_.frameSize += n;
    return at;
}

SubqueryPlan* Planner::newSubquery(SubqueryPlan::Kind kind)
{
    prog_.subqueries.push_back(std::make_unique<SubqueryPlan>());
    prog_.subqueries.back()->kind = kind;
    return prog_.subqueries.back().get();
}

void Planner::conjuncts(Expr* e, std::vector<Expr*>& out)
{
    if (!e)
        return;
    if (e->kind == ExprKind::Binary && BinaryOp(e->op) == BinaryOp::And) {
        conjuncts(e->lhs.get(), out);
        conjuncts(e->rhs.get(), out);
        return;
    }
    out.push_back(e);
}

ExprPtr Planner::copyExpr(const Expr& e, std::string_view text)
{
    ExprPtr copy = parseExpression(text.substr(e.begin, e.end - e.begin));
    copyParams(e, *copy);
    return copy;
}

const Table* Planner::findTable(std::string_view schema, std::string_view name, std::size_t& store)
{
    if (!schema.empty()) {
        const int i = session_.storeIndex(schema);
        if (i < 0)
            throw Error(ErrorCode::NoSuchTable, "unknown database " + std::string(schema));
        store = std::size_t(i);
        return session_.catalog(store).findTable(name);
    }
    for (std::size_t i = 0; i < session_.stores.size(); ++i) {
        // temp (index 1) is searched before main.
        const std::size_t k = i == 0 ? (session_.stores.size() > 1 ? 1 : 0) : (i == 1 ? 0 : i);
        if (!session_.stores[k].catalog)
            continue;
        if (const Table* t = session_.catalog(k).findTable(name)) {
            store = k;
            return t;
        }
    }
    return nullptr;
}

SourceNode& Planner::addTableSource(std::vector<SourceNode>& sources, const Table& t,
                                    std::size_t storeIndex, std::string alias)
{
    SourceNode s;
    s.kind = SourceNode::Kind::Table;
    s.table = &t;
    s.schemaIndex = storeIndex;
    s.name = t.name;
    s.alias = alias.empty() ? t.name : std::move(alias);
    s.schema = session_.stores[storeIndex].name;
    for (const Column& c : t.columns) {
        s.columns.push_back(c.name);
        s.affinity.push_back(c.affinity);
        s.collation.push_back(c.collation);
        s.hidden.push_back(false);
    }
    s.width = t.width();
    s.base = allocate(s.width);
    // VIRTUAL generated columns are computed from the row as it is read.
    bool virtualColumns = false;
    for (const Column& c : t.columns)
        virtualColumns = virtualColumns || c.generated == 1;
    if (virtualColumns) {
        useAllColumns(s, t.columns.size());
        s.generated.resize(t.columns.size());
        std::vector<SourceNode> self;
        SourceNode row;
        row.kind = SourceNode::Kind::Pseudo;
        row.table = &t;
        row.alias = t.name;
        row.columns = s.columns;
        row.affinity = s.affinity;
        row.collation = s.collation;
        row.hidden = s.hidden;
        row.base = s.base;
        row.width = s.width;
        self.push_back(std::move(row));
        for (std::size_t i = 0; i < t.columns.size(); ++i) {
            const Column& c = t.columns[i];
            if (c.generated != 1)
                continue;
            prog_.extraTexts.push_back(std::make_unique<std::string>(c.generatedSql));
            Scope scope;
            scope.sources = &self;
            scope.text = *prog_.extraTexts.back();
            ExprPtr e = parseExpression(c.generatedSql);
            resolve(e, scope, kNoSubquery | kOnlyDeterministic);
            s.generated[i] = std::move(e);
        }
    }
    sources.push_back(std::move(s));
    return sources.back();
}

void Planner::setSourceFromSelect(SourceNode& src, SelectPlan& sp)
{
    std::set<std::string> seen;
    for (std::size_t i = 0; i < sp.columns; ++i) {
        std::string n = sp.subNames[i];
        // Duplicate names in a sub-query become name:1, name:2, ...
        if (seen.count(toLower(n))) {
            for (int k = 1;; ++k) {
                std::string cand = n + ":" + std::to_string(k);
                if (!seen.count(toLower(cand))) {
                    n = cand;
                    break;
                }
            }
        }
        seen.insert(toLower(n));
        src.columns.push_back(n);
        src.affinity.push_back(sp.hasAffinity[i] ? sp.affinity[i] : Affinity::Blob);
        src.collation.push_back(sp.collation[i]);
        src.hidden.push_back(false);
    }
    src.width = sp.columns;
    src.base = allocate(src.width);
}

// ---------------------------------------------------------- resolution ---

namespace {

/// For a column joined by USING/NATURAL into a RIGHT or FULL join, the
/// unqualified name means coalesce(left, right): either side may be the NULL
/// padding. Returns the right side's source, or null when no such link.
const SourceNode* rightLink(const std::vector<SourceNode>& sources, int leftSlot, int& rightSlot)
{
    for (const SourceNode& s : sources) {
        if (s.join != JoinType::Right && s.join != JoinType::Full)
            continue;
        for (const auto& link : s.usingLinks) {
            if (link.second == leftSlot) {
                rightSlot = int(s.base) + link.first;
                return &s;
            }
        }
    }
    return nullptr;
}

ExprPtr coalesceOf(int leftSlot, int rightSlot, const std::string& name)
{
    ExprPtr e = makeExpr(ExprKind::Function);
    e->name = "coalesce";
    e->fn = FunctionRegistry::builtins().find("coalesce", 2);
    e->op = kCoalesce;
    for (const int slot : {leftSlot, rightSlot}) {
        ExprPtr c = makeExpr(ExprKind::Column);
        c->slot = slot;
        c->name = name;
        e->list.push_back(std::move(c));
    }
    return e;
}

}  // namespace

void Planner::computeExprMeta(Expr& e)
{
    switch (e.kind) {
        case ExprKind::Cast:
            e.affinity = affinityOfTypeName(e.name);
            e.hasAffinity = true;
            e.collation = e.lhs->collation;
            e.collationStrength = e.lhs->collationStrength;
            break;
        case ExprKind::Collate: {
            const auto c = collationOfName(e.name);
            if (!c)
                error("no such collation sequence: " + e.name);
            e.collation = *c;
            e.collationStrength = 2;
            e.affinity = e.lhs->affinity;
            e.hasAffinity = e.lhs->hasAffinity;
            e.strictDatetime = e.lhs->strictDatetime;
            break;
        }
        case ExprKind::Unary:
            if (UnaryOp(e.op) == UnaryOp::Plus) {
                e.collation = e.lhs->collation;
                e.collationStrength = e.lhs->collationStrength;
            }
            break;
        case ExprKind::Binary:
            switch (BinaryOp(e.op)) {
                case BinaryOp::Eq:
                case BinaryOp::Ne:
                case BinaryOp::Lt:
                case BinaryOp::Le:
                case BinaryOp::Gt:
                case BinaryOp::Ge: e.collation = comparisonCollation(*e.lhs, *e.rhs); break;
                default: break;
            }
            break;
        case ExprKind::Is: e.collation = comparisonCollation(*e.lhs, *e.rhs); break;
        case ExprKind::Subquery:
            if (e.sub && e.sub->select && e.sub->select->columns > 0) {
                e.affinity = e.sub->select->affinity[0];
                e.hasAffinity = e.sub->select->hasAffinity[0];
                e.collation = e.sub->select->collation[0];
                e.collationStrength = e.sub->select->collationStrength[0];
            }
            break;
        default: break;
    }
}

void Planner::resolve(ExprPtr& ep, Scope& scope, unsigned flags)
{
    Expr& e = *ep;
    switch (e.kind) {
        case ExprKind::Column: resolveColumn(ep, scope, flags); return;
        case ExprKind::Function: resolveFunction(ep, scope, flags); return;
        case ExprKind::Window:
            if (!(flags & kAllowWindow))
                error("misuse of window function " + e.name + "()");
            resolveWindow(e, scope, flags);
            return;
        case ExprKind::Subquery:
        case ExprKind::Exists:
        case ExprKind::InSelect:
            if (flags & kNoSubquery)
                error("subqueries prohibited in this context");
            if (e.lhs)
                resolve(e.lhs, scope, flags);
            planSubqueryExpr(e, scope);
            computeExprMeta(e);
            return;
        case ExprKind::Raise:
            if (!scope.trigger) {
                Scope* s = &scope;
                bool ok = false;
                for (; s; s = s->parent)
                    ok = ok || s->trigger;
                if (!ok)
                    error("RAISE() may only be used within a trigger-program");
            }
            return;
        case ExprKind::Like:
            if (LikeKind(e.op) == LikeKind::Regexp || LikeKind(e.op) == LikeKind::Match) {
                const char* fname = LikeKind(e.op) == LikeKind::Regexp ? "regexp" : "match";
                e.fn = FunctionRegistry::builtins().find(fname, 2);
                if (!e.fn)
                    error(std::string("no such function: ") + (LikeKind(e.op) == LikeKind::Regexp ? "REGEXP" : "MATCH"));
            }
            break;
        default: break;
    }
    if (e.lhs)
        resolve(e.lhs, scope, flags);
    if (e.rhs)
        resolve(e.rhs, scope, flags);
    if (e.extra)
        resolve(e.extra, scope, flags);
    for (ExprPtr& x : e.list)
        resolve(x, scope, flags);
    computeExprMeta(e);
}

void Planner::resolveColumn(ExprPtr& ep, Scope& scope, unsigned flags)
{
    Expr& e = *ep;
    int depth = 0;
    for (Scope* s = &scope; s; s = s->parent, ++depth) {
        if (!s->sources)
            continue;
        const std::size_t limit = std::min(s->visible, s->sources->size());
        SourceNode* found = nullptr;
        int foundCol = -1;
        int matches = 0;
        for (std::size_t i = 0; i < limit; ++i) {
            SourceNode& src = (*s->sources)[i];
            if (src.kind == SourceNode::Kind::Single)
                continue;
            if (src.qualifiedOnly && e.table.empty())
                continue;
            if (!e.table.empty()) {
                if (!equalsNoCase(src.alias, e.table))
                    continue;
                if (!e.schema.empty() && !equalsNoCase(src.schema, e.schema))
                    continue;
            }
            int col = -1;
            for (std::size_t k = 0; k < src.columns.size(); ++k) {
                if (equalsNoCase(src.columns[k], e.name)) {
                    col = int(k);
                    break;
                }
            }
            if (col < 0 && src.table)
                col = src.table->find(e.name);
            if (col < 0)
                continue;
            // A USING/NATURAL column of a later source coalesces with the
            // earlier one: an unqualified name means the earlier.
            if (e.table.empty() && found) {
                bool linked = false;
                for (const auto& link : src.usingLinks) {
                    if (link.first == col)
                        linked = true;
                }
                if (linked)
                    continue;
            }
            ++matches;
            if (!found) {
                found = &src;
                foundCol = col;
            }
        }
        if (matches > 1)
            throw Error(ErrorCode::AmbiguousColumn, "ambiguous column name: " + e.describe());
        if (!found)
            continue;
        e.slot = int(found->base) + foundCol;
        if (foundCol < int(found->columns.size())) {
            e.affinity = found->affinity[std::size_t(foundCol)];
            e.hasAffinity = true;
            e.collation = found->collation[std::size_t(foundCol)];
            e.collationStrength = 1;
            if (found->table && found->table->strict &&
                found->table->columns[std::size_t(foundCol)].strictType == StrictType::Datetime)
                e.strictDatetime = true;
            if (found->kind == SourceNode::Kind::Table)
                useColumn(*found, std::size_t(foundCol));
            if (found->table && found->table->rowidAlias == foundCol)
                found->needRowid = true;
        } else {
            e.affinity = Affinity::Integer;
            e.hasAffinity = true;
            found->needRowid = true;
        }
        if (e.table.empty()) {
            int rightSlot = -1;
            if (rightLink(*s->sources, e.slot, rightSlot)) {
                const std::string nm = e.name;
                const int left = e.slot;
                ep = coalesceOf(left, rightSlot, nm);
                ep->begin = e.begin;
                ep->end = e.end;
            }
        }
        // An outer reference makes every sub-query between here and there
        // correlated.
        if (depth > 0) {
            for (Scope* t = &scope; t && t != s; t = t->parent) {
                if (t->owner)
                    t->owner->correlated = true;
            }
        }
        return;
    }
    // A result alias may stand in for its expression outside the select list.
    if ((flags & kAllowAlias) && e.table.empty() && scope.resultColumns) {
        for (ResultColumn& rc : *scope.resultColumns) {
            if (rc.expr && !rc.alias.empty() && equalsNoCase(rc.alias, e.name)) {
                ExprPtr copy = copyExpr(*rc.expr, scope.text);
                ep = std::move(copy);
                resolve(ep, scope, flags & ~kAllowAlias);
                return;
            }
        }
    }
    noColumn(e.describe());
}

void Planner::resolveFunction(ExprPtr& ep, Scope& scope, unsigned flags)
{
    Expr& e = *ep;
    const FunctionRegistry& reg = FunctionRegistry::builtins();
    const int argc = int(e.list.size());
    const FunctionDef* def = reg.find(e.name, argc);
    if (!def) {
        std::uint8_t code = 0;
        if (isWindowBuiltin(e.name, code))
            error("misuse of window function " + toLower(e.name) + "()");
        if (reg.hasName(e.name))
            error("wrong number of arguments to function " + e.name + "()");
        throw Error(ErrorCode::Unsupported, "no such function: " + e.name);
    }
    if (e.star && !(def->aggregate && equalsNoCase(e.name, "count")))
        error("wrong number of arguments to function " + e.name + "()");
    if ((flags & kOnlyDeterministic) && !(def->flags & kDeterministic))
        error("non-deterministic functions prohibited in this context");
    if (def->aggregate) {
        if (flags & kInAggregate)
            error("misuse of aggregate function " + e.name + "()");
        if (!(flags & kAllowAggregate) || !scope.core)
            error("misuse of aggregate function " + e.name + "()");
        if (e.distinct && argc != 1)
            error("DISTINCT aggregates must have exactly one argument");
        const unsigned inner = (flags & ~(kAllowWindow | kAllowAlias)) | kInAggregate;
        for (ExprPtr& x : e.list)
            resolve(x, scope, inner);
        if (e.filter)
            resolve(e.filter, scope, inner);
        for (OrderTerm& t : e.orderBy)
            resolve(t.expr, scope, inner);
        e.kind = ExprKind::Aggregate;
        e.fn = def;
        e.slot = int(allocate(1));
        if (!e.list.empty() && e.list[0]->collationStrength)
            e.collation = e.list[0]->collation;
        AggInfo info;
        info.call = &e;
        info.def = def;
        info.countStar = e.star;
        info.minMax = argc == 1 && (equalsNoCase(e.name, "min") || equalsNoCase(e.name, "max"));
        e.aggIndex = int(scope.core->aggs.size());
        scope.core->aggs.push_back(info);
        return;
    }
    if (e.distinct)
        error("DISTINCT aggregates must have exactly one argument");
    if (e.filter)
        error("FILTER may not be used with non-aggregate " + e.name + "()");
    if (!e.orderBy.empty())
        error("ORDER BY may not be used with non-aggregate " + e.name + "()");
    for (ExprPtr& x : e.list)
        resolve(x, scope, flags);
    e.fn = def;
    const std::string lname = toLower(e.name);
    if (lname == "coalesce" || lname == "ifnull")
        e.op = kCoalesce;
    else if (lname == "iif")
        e.op = kIif;
    else if (lname == "likely" || lname == "unlikely" || lname == "likelihood")
        e.op = kIdentity;
    // The collation a text-comparing function uses: the first argument's
    // explicit or column collation.
    for (const ExprPtr& x : e.list) {
        if (x->collationStrength == 2) {
            e.collation = x->collation;
            break;
        }
    }
    if (e.collation == Collation::Binary) {
        for (const ExprPtr& x : e.list) {
            if (x->collationStrength == 1) {
                e.collation = x->collation;
                break;
            }
        }
    }
    if (e.op == kIdentity && !e.list.empty()) {
        e.affinity = e.list[0]->affinity;
        e.hasAffinity = e.list[0]->hasAffinity;
    }
}

void Planner::resolveWindow(Expr& e, Scope& scope, unsigned flags)
{
    if (!scope.core)
        error("misuse of window function " + e.name + "()");
    CorePlan& core = *scope.core;
    WinInfo w;
    w.call = &e;
    std::uint8_t code = 0;
    const int argc = int(e.list.size());
    if (isWindowBuiltin(e.name, code)) {
        windowArity(code, e.list.size(), e.name);
        w.builtin = code;
        if (e.filter)
            error("FILTER clause may only be used with aggregate window functions");
    } else {
        const FunctionDef* def = FunctionRegistry::builtins().find(e.name, argc);
        if (!def) {
            if (FunctionRegistry::builtins().hasName(e.name))
                error("wrong number of arguments to function " + e.name + "()");
            throw Error(ErrorCode::Unsupported, "no such function: " + e.name);
        }
        if (!def->aggregate)
            error(e.name + "() may not be used as a window function");
        if (e.distinct)
            error("DISTINCT is not supported for window functions");
        w.def = def;
        e.fn = def;
    }
    const unsigned inner = (flags & ~(kAllowWindow | kAllowAlias)) | kAllowAggregate;
    for (ExprPtr& x : e.list)
        resolve(x, scope, inner);
    if (e.filter)
        resolve(e.filter, scope, inner);
    if (!e.list.empty() && e.list[0]->collationStrength)
        e.collation = e.list[0]->collation;

    // Merge a named window: OVER name, or OVER (name ...).
    WindowSpec& spec = *e.window;
    if (!spec.base.empty()) {
        const WindowSpec* base = nullptr;
        if (core.core) {
            for (const NamedWindow& nw : core.core->windows) {
                if (equalsNoCase(nw.name, spec.base))
                    base = &nw.spec;
            }
        }
        if (!base)
            error("no such window: " + spec.base);
        const bool bare = e.literal.type() == Type::Integer;
        if (!bare && !spec.partition.empty())
            error("cannot override PARTITION clause of window " + spec.base);
        if (!bare && !base->order.empty() && !spec.order.empty())
            error("cannot override ORDER BY clause of window " + spec.base);
        if (!bare && base->hasFrame)
            error("cannot override frame specification of window " + spec.base);
        // Copy the base's clauses into this spec (re-parsed copies).
        for (const ExprPtr& p : base->partition)
            spec.partition.push_back(copyExpr(*p, scope.text));
        if (spec.order.empty()) {
            for (const OrderTerm& t : base->order) {
                OrderTerm c;
                c.expr = copyExpr(*t.expr, scope.text);
                c.desc = t.desc;
                c.nulls = t.nulls;
                spec.order.push_back(std::move(c));
            }
        }
        if (bare || (!spec.hasFrame && base->hasFrame)) {
            spec.hasFrame = base->hasFrame;
            spec.unit = base->unit;
            spec.startKind = base->startKind;
            spec.endKind = base->endKind;
            if (base->startExpr)
                spec.startExpr = copyExpr(*base->startExpr, scope.text);
            if (base->endExpr)
                spec.endExpr = copyExpr(*base->endExpr, scope.text);
            spec.exclude = base->exclude;
        }
        spec.base.clear();
    }
    for (ExprPtr& p : spec.partition)
        resolve(p, scope, inner);
    for (OrderTerm& t : spec.order)
        resolve(t.expr, scope, inner);
    if (spec.startExpr)
        resolve(spec.startExpr, scope, inner);
    if (spec.endExpr)
        resolve(spec.endExpr, scope, inner);
    if (spec.hasFrame) {
        if (spec.startKind == FrameBound::UnboundedFollowing ||
            spec.endKind == FrameBound::UnboundedPreceding ||
            (spec.startKind == FrameBound::CurrentRow && spec.endKind == FrameBound::Preceding) ||
            (spec.startKind == FrameBound::Following &&
             (spec.endKind == FrameBound::Preceding || spec.endKind == FrameBound::CurrentRow)))
            error("unsupported frame specification");
        if (spec.unit == FrameUnit::Range &&
            (spec.startKind == FrameBound::Preceding || spec.startKind == FrameBound::Following ||
             spec.endKind == FrameBound::Preceding || spec.endKind == FrameBound::Following) &&
            spec.order.size() != 1)
            error("RANGE with offset PRECEDING/FOLLOWING requires exactly one ORDER BY term");
    }
    e.slot = int(allocate(1));
    // Windows with identical PARTITION BY/ORDER BY/frame share one sort.
    w.spec = &spec;
    w.specIndex = core.windowSpecs.size();
    for (std::size_t i = 0; i < core.windowSpecs.size(); ++i) {
        const WindowSpec& o = *core.windowSpecs[i];
        bool same = o.partition.size() == spec.partition.size() && o.order.size() == spec.order.size();
        for (std::size_t k = 0; same && k < o.partition.size(); ++k)
            same = sameExpr(*o.partition[k], *spec.partition[k]);
        for (std::size_t k = 0; same && k < o.order.size(); ++k)
            same = sameExpr(*o.order[k].expr, *spec.order[k].expr) && o.order[k].desc == spec.order[k].desc &&
                   o.order[k].nulls == spec.order[k].nulls;
        if (same) {
            w.specIndex = i;
            break;
        }
    }
    if (w.specIndex == core.windowSpecs.size())
        core.windowSpecs.push_back(&spec);
    core.windows.push_back(w);
}

void Planner::planSubqueryExpr(Expr& e, Scope& scope)
{
    SubqueryPlan::Kind kind = e.kind == ExprKind::Exists   ? SubqueryPlan::Kind::Exists
                              : e.kind == ExprKind::InSelect ? SubqueryPlan::Kind::In
                                                             : SubqueryPlan::Kind::Scalar;
    SubqueryPlan* sub = newSubquery(kind);
    sub->select = planSelect(*e.select, &scope, scope.text, sub);
    sub->width = sub->select->columns;
    e.sub = sub;
    if (kind == SubqueryPlan::Kind::Scalar) {
        // A multi-column scalar sub-query is only valid as a row value.
        return;
    }
    if (kind == SubqueryPlan::Kind::In) {
        std::size_t lhsWidth = 1;
        std::vector<const Expr*> lhs;
        if (e.lhs->kind == ExprKind::RowValue) {
            lhsWidth = e.lhs->list.size();
            for (const ExprPtr& x : e.lhs->list)
                lhs.push_back(x.get());
        } else {
            lhs.push_back(e.lhs.get());
        }
        if (lhsWidth != sub->width)
            error("sub-select returns " + std::to_string(sub->width) + " columns - expected " +
                  std::to_string(lhsWidth));
        for (std::size_t i = 0; i < lhsWidth; ++i) {
            Expr rhs;
            rhs.affinity = sub->select->affinity[i];
            rhs.hasAffinity = sub->select->hasAffinity[i];
            rhs.collation = sub->select->collation[i];
            rhs.collationStrength = sub->select->collationStrength[i];
            sub->keyAffinity.push_back(comparisonAffinity(*lhs[i], rhs));
            sub->rowAffinity.push_back(comparisonAffinity(rhs, *lhs[i]));
            // For IN, the left operand's collation decides.
            sub->keyCollation.push_back(lhs[i]->collationStrength ? lhs[i]->collation
                                                                  : comparisonCollation(*lhs[i], rhs));
        }
    }
}

// --------------------------------------------------------------- FROM ----

SubqueryPlan* Planner::planCte(CteDef& d, Scope& scope)
{
    if (d.plan)
        return d.plan;
    SubqueryPlan* sub = newSubquery(SubqueryPlan::Kind::Rows);
    d.plan = sub;
    d.planning = true;
    SelectStmt& s = *d.cte->select;
    Scope* home = d.home ? d.home : &scope;

    std::size_t firstRecursive = s.cores.size();
    if (d.recursive) {
        std::function<bool(const std::vector<FromItem>&)> refs = [&](const std::vector<FromItem>& from) {
            for (const FromItem& f : from) {
                if (f.kind == FromItem::Kind::Table && f.schema.empty() &&
                    equalsNoCase(f.name, d.cte->name))
                    return true;
                if (f.kind == FromItem::Kind::Group && refs(f.group))
                    return true;
            }
            return false;
        };
        for (std::size_t i = 0; i < s.cores.size(); ++i) {
            if (refs(s.cores[i].from)) {
                firstRecursive = i;
                break;
            }
        }
    }

    if (firstRecursive == s.cores.size()) {
        sub->select = planSelect(s, home, d.text, sub);
        sub->width = sub->select->columns;
    } else {
        if (firstRecursive == 0)
            error("recursive reference in a subquery: " + d.cte->name);
        if (s.ops[firstRecursive - 1] != CompoundOp::UnionAll &&
            s.ops[firstRecursive - 1] != CompoundOp::Union)
            error("recursive reference in a subquery: " + d.cte->name);
        // Split into the initial select and the recursive one.
        auto initial = std::make_unique<SelectStmt>();
        auto rec = std::make_unique<SelectStmt>();
        for (std::size_t i = 0; i < s.cores.size(); ++i) {
            if (i < firstRecursive) {
                initial->cores.push_back(std::move(s.cores[i]));
                if (i > 0)
                    initial->ops.push_back(s.ops[i - 1]);
            } else {
                rec->cores.push_back(std::move(s.cores[i]));
                if (i > firstRecursive)
                    rec->ops.push_back(s.ops[i - 1]);
            }
        }
        sub->recursiveUnionAll = s.ops[firstRecursive - 1] == CompoundOp::UnionAll;
        for (CompoundOp& op : rec->ops)
            op = CompoundOp::UnionAll;
        sub->select = planSelect(*initial, home, d.text, sub);
        sub->width = sub->select->columns;
        d.names = d.cte->columns.empty() ? sub->select->subNames : d.cte->columns;
        if (d.names.size() != sub->width)
            error("table " + d.cte->name + " has " + std::to_string(sub->width) +
                  " values for " + std::to_string(d.names.size()) + " columns");
        sub->recursive = planSelect(*rec, home, d.text, sub);
        if (sub->recursive->columns != sub->width)
            error("SELECTs to the left and right of UNION do not have the same number of result columns");
        // ORDER BY and LIMIT of a recursive CTE govern the queue.
        for (OrderTerm& t : s.order) {
            std::size_t col = sub->width;
            if (t.expr->kind == ExprKind::Literal && t.expr->literal.type() == Type::Integer) {
                const std::int64_t k = t.expr->literal.integer();
                if (k >= 1 && std::size_t(k) <= sub->width)
                    col = std::size_t(k - 1);
            } else if (t.expr->kind == ExprKind::Column && t.expr->table.empty()) {
                for (std::size_t i = 0; i < d.names.size(); ++i) {
                    if (equalsNoCase(d.names[i], t.expr->name))
                        col = i;
                }
            }
            if (col >= sub->width)
                error("ORDER BY term of a recursive CTE must name a result column");
            sub->queueOrder.push_back(col);
            sub->queueDesc.push_back(t.desc);
        }
        Scope none;
        none.parent = home;
        none.text = d.text;
        if (s.limit) {
            resolve(s.limit, none, 0);
            sub->limit = s.limit.get();
        }
        if (s.offset) {
            resolve(s.offset, none, 0);
            sub->offset = s.offset.get();
        }
        prog_.ownedSelects.push_back(std::move(initial));
        prog_.ownedSelects.push_back(std::move(rec));
    }
    if (!d.cte->columns.empty()) {
        if (d.cte->columns.size() != sub->width)
            error("table " + d.cte->name + " has " + std::to_string(sub->width) +
                  " values for " + std::to_string(d.cte->columns.size()) + " columns");
        for (std::size_t i = 0; i < sub->width; ++i)
            sub->select->subNames[i] = d.cte->columns[i];
    }
    d.planning = false;
    return sub;
}

SubqueryPlan* Planner::planView(const View& v, std::size_t store)
{
    (void)store;
    for (const std::string& n : viewsInProgress_) {
        if (equalsNoCase(n, v.name))
            error("view " + v.name + " is circularly defined");
    }
    viewsInProgress_.push_back(v.name);
    prog_.extraTexts.push_back(std::make_unique<std::string>(v.sql));
    const std::string& text = *prog_.extraTexts.back();
    prog_.extraAsts.push_back(std::make_unique<StatementNode>(parseOne(text)));
    auto& def = std::get<CreateViewStmt>(prog_.extraAsts.back()->body);
    SubqueryPlan* sub = newSubquery(SubqueryPlan::Kind::Rows);
    sub->select = planSelect(*def.select, nullptr, text, sub);
    sub->width = sub->select->columns;
    if (!def.columns.empty()) {
        if (def.columns.size() != sub->width)
            error("expected " + std::to_string(def.columns.size()) + " columns for '" + v.name +
                  "' but got " + std::to_string(sub->width));
        for (std::size_t i = 0; i < sub->width; ++i)
            sub->select->subNames[i] = def.columns[i];
    }
    viewsInProgress_.pop_back();
    return sub;
}

void Planner::addSource(FromItem& item, CorePlan& core, Scope& scope)
{
    if (item.kind == FromItem::Kind::Group) {
        const std::size_t first = core.sources.size();
        for (FromItem& g : item.group)
            addSource(g, core, scope);
        if (core.sources.size() > first)
            core.sources[first].join = item.join;
        return;
    }
    SourceNode src;
    src.join = item.join;
    src.indexedBy = item.indexedBy;
    src.notIndexed = item.notIndexed;
    if (item.kind == FromItem::Kind::Subquery) {
        src.kind = SourceNode::Kind::Rows;
        SubqueryPlan* sub = newSubquery(SubqueryPlan::Kind::Rows);
        sub->select = planSelect(*item.subquery, scope.parent, scope.text, sub);
        sub->width = sub->select->columns;
        src.rows = sub;
        src.alias = item.alias;
        src.name = item.alias;
        setSourceFromSelect(src, *sub->select);
        core.sources.push_back(std::move(src));
        return;
    }
    if (item.kind == FromItem::Kind::Function) {
        if (item.name.size() > 7 && equalsNoCase(item.name.substr(0, 7), "pragma_")) {
            SubqueryPlan* sub = planPragmaFunction(item.name.substr(7), item.args, scope);
            src.kind = SourceNode::Kind::Rows;
            src.rows = sub;
            src.name = item.name;
            src.alias = item.alias.empty() ? item.name : item.alias;
            setSourceFromSelect(src, *sub->select);
            for (std::size_t k = 0; k < src.columns.size(); ++k)
                src.hidden[k] = src.columns[k] == "arg" || src.columns[k] == "schema";
            core.sources.push_back(std::move(src));
            return;
        }
        const TableFunctionDef* fn = FunctionRegistry::builtins().findTable(item.name);
        if (!fn)
            throw Error(ErrorCode::NoSuchTable, "no such table: " + item.name);
        if (int(item.args.size()) > fn->maxArgs)
            error("too many arguments on " + item.name + "() - max " + std::to_string(fn->maxArgs));
        if (int(item.args.size()) < fn->minArgs)
            error("too few arguments on " + item.name + "()");
        src.kind = SourceNode::Kind::Function;
        src.function = fn;
        src.name = item.name;
        src.alias = item.alias.empty() ? item.name : item.alias;
        for (std::size_t i = 0; i < fn->columns.size(); ++i) {
            src.columns.push_back(fn->columns[i]);
            src.affinity.push_back(Affinity::Blob);
            src.collation.push_back(Collation::Binary);
            src.hidden.push_back(int(i) >= fn->hiddenStart);
        }
        src.width = fn->columns.size();
        src.base = allocate(src.width);
        // Arguments may read the sources to the left (and outer queries).
        const std::size_t saved = scope.visible;
        scope.visible = core.sources.size();
        for (ExprPtr& a : item.args) {
            resolve(a, scope, 0);
            src.args.push_back(a.get());
        }
        scope.visible = saved;
        core.sources.push_back(std::move(src));
        return;
    }

    // A CTE in scope, a table, a view, or a schema table.
    CteDef* cte = nullptr;
    if (item.schema.empty()) {
        for (Scope* s = &scope; s && !cte; s = s->parent) {
            for (CteDef* d : s->ctes) {
                if (equalsNoCase(d->cte->name, item.name)) {
                    cte = d;
                    break;
                }
            }
        }
    }
    if (cte) {
        src.name = cte->cte->name;
        src.alias = item.alias.empty() ? cte->cte->name : item.alias;
        if (cte->planning) {
            if (!cte->recursive || cte->names.empty())
                error("circular reference: " + cte->cte->name);
            src.kind = SourceNode::Kind::Recursive;
            src.rows = cte->plan;
            for (const std::string& n : cte->names) {
                src.columns.push_back(n);
                src.affinity.push_back(Affinity::Blob);
                src.collation.push_back(Collation::Binary);
                src.hidden.push_back(false);
            }
            src.width = cte->names.size();
            src.base = allocate(src.width);
            core.sources.push_back(std::move(src));
            return;
        }
        SubqueryPlan* sub = planCte(*cte, scope);
        src.kind = SourceNode::Kind::Rows;
        src.rows = sub;
        setSourceFromSelect(src, *sub->select);
        core.sources.push_back(std::move(src));
        return;
    }
    std::size_t store = 0;
    if (item.schema.empty() && item.name.size() > 7 && equalsNoCase(item.name.substr(0, 7), "pragma_") &&
        !findTable(item.schema, item.name, store)) {
        // A table-valued pragma may be named without its parentheses.
        item.kind = FromItem::Kind::Function;
        addSource(item, core, scope);
        return;
    }
    if (const Table* t = findTable(item.schema, item.name, store)) {
        SourceNode& added = addTableSource(core.sources, *t, store, item.alias);
        added.join = item.join;
        added.indexedBy = item.indexedBy;
        added.notIndexed = item.notIndexed;
        if (!item.indexedBy.empty()) {
            bool ok = false;
            for (const Index* ix : t->indexes)
                ok = ok || equalsNoCase(ix->name, item.indexedBy);
            if (!ok)
                error("no such index: " + item.indexedBy);
        }
        return;
    }
    for (std::size_t i = 0; i < session_.stores.size(); ++i) {
        if (!session_.stores[i].catalog)
            continue;
        if (!item.schema.empty() && !equalsNoCase(session_.stores[i].name, item.schema))
            continue;
        if (const View* view = session_.catalog(i).findView(item.name)) {
            SubqueryPlan* sub = planView(*view, i);
            src.kind = SourceNode::Kind::Rows;
            src.rows = sub;
            src.name = view->name;
            src.alias = item.alias.empty() ? view->name : item.alias;
            setSourceFromSelect(src, *sub->select);
            core.sources.push_back(std::move(src));
            return;
        }
    }
    if (SubqueryPlan* sys = planSchemaTable(item.schema, item.name)) {
        src.kind = SourceNode::Kind::Rows;
        src.rows = sys;
        src.name = item.name;
        src.alias = item.alias.empty() ? item.name : item.alias;
        setSourceFromSelect(src, *sys->select);
        core.sources.push_back(std::move(src));
        return;
    }
    throw Error(ErrorCode::NoSuchTable,
                "no such table: " + (item.schema.empty() ? "" : item.schema + ".") + item.name);
}

void Planner::planFrom(std::vector<FromItem>& from, CorePlan& core, Scope& scope)
{
    core.slotBegin = prog_.frameSize;
    // Flatten parenthesized joins, remembering which FromItem each source
    // came from so its ON/USING can be applied.
    struct Entry
    {
        std::size_t source;
        FromItem* item;
    };
    std::vector<Entry> entries;
    // A parenthesized join contributes its own sources (and their ON
    // clauses); the group's join type goes to its first source and the
    // group's ON to its last.
    std::vector<std::pair<std::size_t, FromItem*>> groupOns;
    std::function<void(std::vector<FromItem>&)> walk = [&](std::vector<FromItem>& items) {
        for (FromItem& f : items) {
            const std::size_t before = core.sources.size();
            if (f.kind == FromItem::Kind::Group) {
                walk(f.group);
                if (core.sources.size() > before) {
                    core.sources[before].join = f.join;
                    if (f.on || !f.usingColumns.empty() || f.natural)
                        groupOns.emplace_back(core.sources.size() - 1, &f);
                }
                continue;
            }
            addSource(f, core, scope);
            if (core.sources.size() > before)
                entries.push_back({core.sources.size() - 1, &f});
        }
    };
    walk(from);
    for (auto& g : groupOns)
        entries.push_back({g.first, g.second});
    core.slotEnd = prog_.frameSize;
    for (SourceNode& s : core.sources) {
        if (s.join == JoinType::Right || s.join == JoinType::Full)
            core.hasRightJoin = true;
    }
    for (const Entry& en : entries) {
        const std::size_t d = en.source;
        FromItem& item = *en.item;
        SourceNode& right = core.sources[d];
        // USING / NATURAL: equality terms plus coalescing links.
        std::vector<std::string> names = item.usingColumns;
        if (item.natural) {
            for (std::size_t k = 0; k < right.columns.size(); ++k) {
                if (right.hidden[k])
                    continue;
                bool found = false;
                for (std::size_t l = 0; l < d && !found; ++l) {
                    const SourceNode& left = core.sources[l];
                    for (std::size_t m = 0; m < left.columns.size(); ++m)
                        found = found || (!left.hidden[m] && equalsNoCase(left.columns[m], right.columns[k]));
                }
                if (found)
                    names.push_back(right.columns[k]);
            }
        }
        for (const std::string& n : names) {
            int rcol = -1;
            for (std::size_t k = 0; k < right.columns.size(); ++k) {
                if (equalsNoCase(right.columns[k], n)) {
                    rcol = int(k);
                    break;
                }
            }
            SourceNode* lsrc = nullptr;
            int lcol = -1;
            for (std::size_t l = 0; l < d && !lsrc; ++l) {
                SourceNode& left = core.sources[l];
                for (std::size_t m = 0; m < left.columns.size(); ++m) {
                    if (equalsNoCase(left.columns[m], n)) {
                        lsrc = &left;
                        lcol = int(m);
                        break;
                    }
                }
            }
            if (rcol < 0 || !lsrc)
                error("cannot join using column " + n + " - column not present in both tables");
            right.usingLinks.emplace_back(rcol, int(lsrc->base) + lcol);
            auto column = [](SourceNode& src, int col, const std::string& nm) {
                ExprPtr c = makeExpr(ExprKind::Column);
                c->slot = int(src.base) + col;
                c->name = nm;
                c->affinity = src.affinity[std::size_t(col)];
                c->hasAffinity = true;
                c->collation = src.collation[std::size_t(col)];
                c->collationStrength = 1;
                if (src.kind == SourceNode::Kind::Table)
                    useColumn(src, std::size_t(col));
                return c;
            };
            ExprPtr eq = makeExpr(ExprKind::Binary);
            eq->op = std::uint8_t(BinaryOp::Eq);
            eq->lhs = column(*lsrc, lcol, n);
            eq->rhs = column(right, rcol, n);
            eq->collation = comparisonCollation(*eq->lhs, *eq->rhs);
            right.onTerms.push_back(eq.get());
            prog_.owned.push_back(std::move(eq));
        }
        if (item.on) {
            if (d == 0)
                error("a JOIN clause is required before ON");
            const std::size_t saved = scope.visible;
            scope.visible = d + 1;
            resolve(item.on, scope, 0);
            scope.visible = saved;
            if (containsKind(item.on.get(), ExprKind::Aggregate))
                error("misuse of aggregate function in ON clause");
            conjuncts(item.on.get(), right.onTerms);
        }
    }
}

namespace {

/// The source (by position in `core.sources`) that holds `slot`, or -1.
int sourceOfSlot(const CorePlan& core, int slot)
{
    for (std::size_t d = 0; d < core.sources.size(); ++d) {
        const SourceNode& s = core.sources[d];
        if (std::size_t(slot) >= s.base && std::size_t(slot) < s.base + s.width)
            return int(d);
    }
    return -1;
}

/// The sources an expression reads, as a bit set over core.sources positions
/// (a correlated sub-query reads them all).
void sourcesRead(const CorePlan& core, const Expr* x, std::vector<bool>& out, bool& all)
{
    if (!x)
        return;
    if (x->kind == ExprKind::Column || x->kind == ExprKind::Slot) {
        const int d = sourceOfSlot(core, x->slot);
        if (d >= 0)
            out[std::size_t(d)] = true;
    }
    if (x->sub && x->sub->correlated)
        all = true;
    sourcesRead(core, x->lhs.get(), out, all);
    sourcesRead(core, x->rhs.get(), out, all);
    sourcesRead(core, x->extra.get(), out, all);
    for (const ExprPtr& y : x->list)
        sourcesRead(core, y.get(), out, all);
}

/// Inner joins may run in any order; anything else keeps the written order.
bool reorderable(const CorePlan& core)
{
    if (core.sources.size() < 2 || core.hasRightJoin)
        return false;
    for (std::size_t d = 0; d < core.sources.size(); ++d) {
        const SourceNode& s = core.sources[d];
        if (s.kind != SourceNode::Kind::Table && s.kind != SourceNode::Kind::Rows)
            return false;
        if (d > 0 && s.join != JoinType::Inner)
            return false;
    }
    return true;
}

}  // namespace

namespace {

/// Marks inner table sources whose rows do not depend on earlier sources.
void markCacheable(CorePlan& core)
{
    const std::size_t n = core.sources.size();
    ReadySet constantsOnly;
    constantsOnly.outer = 0;
    for (std::size_t d = 1; d < n; ++d) {
        SourceNode& s = core.sources[d];
        s.cacheable = false;
        s.localFilters.clear();
        s.joinFilters.clear();
        if (s.kind != SourceNode::Kind::Table || s.join != JoinType::Inner || !s.onTerms.empty())
            continue;
        const AccessPath& ap = s.access;
        auto constant = [&](const Expr* e) {
            if (!e)
                return true;
            std::vector<bool> r(n, false);
            bool all = false;
            sourcesRead(core, e, r, all);
            if (all)
                return false;
            for (bool b : r) {
                if (b)
                    return false;
            }
            // Outer-scope slots change between runs of a correlated query.
            bool outer = false;
            std::function<void(const Expr*)> walk = [&](const Expr* x) {
                if (!x)
                    return;
                if ((x->kind == ExprKind::Column || x->kind == ExprKind::Slot) &&
                    std::size_t(x->slot) < core.slotBegin)
                    outer = true;
                if (x->kind == ExprKind::Function && x->fn && !(x->fn->flags & kDeterministic))
                    outer = true;
                if (x->sub && x->sub->correlated)
                    outer = true;
                walk(x->lhs.get());
                walk(x->rhs.get());
                walk(x->extra.get());
                for (const ExprPtr& y : x->list)
                    walk(y.get());
            };
            walk(e);
            return !outer;
        };
        bool ok = constant(ap.low) && constant(ap.high) && constant(ap.inList);
        for (const Expr* e : ap.equals)
            ok = ok && constant(e);
        if (!ok)
            continue;
        for (Expr* t : core.levelFilters[d]) {
            std::vector<bool> r(n, false);
            bool all = false;
            sourcesRead(core, t, r, all);
            bool local = !all;
            for (std::size_t o = 0; o < n && local; ++o)
                local = o == d || !r[o];
            // Local and free of outer references and volatile calls.
            if (local) {
                std::vector<bool> none(n, false);
                (void)none;
                bool outerFree = true;
                std::function<void(const Expr*)> walk = [&](const Expr* x) {
                    if (!x)
                        return;
                    if ((x->kind == ExprKind::Column || x->kind == ExprKind::Slot) &&
                        std::size_t(x->slot) < core.slotBegin)
                        outerFree = false;
                    if (x->kind == ExprKind::Function && x->fn && !(x->fn->flags & kDeterministic))
                        outerFree = false;
                    if (x->sub && x->sub->correlated)
                        outerFree = false;
                    if (x->kind == ExprKind::Raise)
                        outerFree = false;
                    walk(x->lhs.get());
                    walk(x->rhs.get());
                    walk(x->extra.get());
                    for (const ExprPtr& y : x->list)
                        walk(y.get());
                };
                walk(t);
                local = outerFree;
            }
            (local ? s.localFilters : s.joinFilters).push_back(t);
        }
        s.cacheable = true;
        // Equality terms between this source and earlier ones key a hash
        // over the cache. Both sides deterministic and free of sub-queries,
        // so evaluating each once per row it belongs to changes nothing.
        s.hashInner.clear();
        s.hashOuter.clear();
        s.hashInnerAffinity.clear();
        s.hashOuterAffinity.clear();
        s.hashCollation.clear();
        auto plainSide = [&](const Expr* x) {
            bool plain = true;
            std::function<void(const Expr*)> walk = [&](const Expr* y) {
                if (!y || !plain)
                    return;
                if (y->sub || y->select || y->kind == ExprKind::Raise || y->kind == ExprKind::RowValue ||
                    (y->kind == ExprKind::Function && (!y->fn || !(y->fn->flags & kDeterministic))) ||
                    ((y->kind == ExprKind::Column || y->kind == ExprKind::Slot) && std::size_t(y->slot) < core.slotBegin))
                    plain = false;
                walk(y->lhs.get());
                walk(y->rhs.get());
                walk(y->extra.get());
                for (const ExprPtr& z : y->list)
                    walk(z.get());
            };
            walk(x);
            return plain && !x->strictDatetime;
        };
        auto readsOnly = [&](const Expr* x, bool self) {
            std::vector<bool> r(n, false);
            bool all = false;
            sourcesRead(core, x, r, all);
            if (all)
                return false;
            bool any = false;
            for (std::size_t o = 0; o < n; ++o) {
                if (!r[o])
                    continue;
                if (self ? o != d : o >= d)
                    return false;
                any = true;
            }
            return any;
        };
        for (const Expr* t : s.joinFilters) {
            if (t->kind != ExprKind::Binary || BinaryOp(t->op) != BinaryOp::Eq)
                continue;
            const Expr* a = t->lhs.get();
            const Expr* b = t->rhs.get();
            if (!plainSide(a) || !plainSide(b))
                continue;
            if (readsOnly(b, true) && readsOnly(a, false))
                std::swap(a, b);
            else if (!(readsOnly(a, true) && readsOnly(b, false)))
                continue;
            s.hashInner.push_back(a);
            s.hashOuter.push_back(b);
            s.hashInnerAffinity.push_back(comparisonAffinity(*a, *b));
            s.hashOuterAffinity.push_back(comparisonAffinity(*b, *a));
            s.hashCollation.push_back(t->collation);
        }
    }
}

/// One part of the key a table source's access path walks, in key order.
/// `plain`: ascending and BINARY, so key order is SQL order for it.
struct WalkedPart
{
    int column;  ///< the rowid is the column past the table's last
    bool plain;
    bool rowKey;  ///< the row key an index entry ends with
};

/// The parts of the key or index `s` reads, up to the first expression part;
/// an index's entries then continue with the row key.
std::vector<WalkedPart> walkedParts(const SourceNode& s)
{
    const Table& t = *s.table;
    const int rowid = int(t.columns.size());
    std::vector<WalkedPart> parts;
    auto key = [&](bool rowKey) {
        if (t.keyedByRowid()) {
            parts.push_back({rowid, true, rowKey});
            return;
        }
        for (std::size_t i = 0; i < t.primaryKey.size(); ++i)
            parts.push_back(
                {t.primaryKey[i], t.pkCollation[i] == Collation::Binary && !t.pkDesc[i], rowKey});
    };
    switch (s.access.kind) {
        case AccessPath::Kind::Scan: break;
        case AccessPath::Kind::Rowid:
        case AccessPath::Kind::Key: key(false); break;
        case AccessPath::Kind::Index: {
            bool expression = false;
            for (const IndexPart& p : s.access.index->parts) {
                expression = p.column == -1;
                if (expression)
                    break;
                parts.push_back({p.column == -2 ? rowid : p.column,
                                 p.collation == Collation::Binary && !p.desc, false});
            }
            if (!expression)
                key(true);
            break;
        }
    }
    for (WalkedPart& p : parts) {
        if (t.rowidAlias >= 0 && p.column == t.rowidAlias)
            p.column = rowid;
    }
    return parts;
}

/// The column of `s` that `e` is a bare reference to (the rowid alias as the
/// rowid), or -1.
int walkedColumn(const SourceNode& s, const Expr* e)
{
    if (e->kind != ExprKind::Column || e->slot < 0 || std::size_t(e->slot) < s.base ||
        std::size_t(e->slot) >= s.base + s.width)
        return -1;
    const int col = int(std::size_t(e->slot) - s.base);
    return s.table->rowidAlias >= 0 && col == s.table->rowidAlias ? int(s.table->columns.size())
                                                                  : col;
}

/// A lone table source whose access path is a plain window: Key, Rowid or
/// Index access with no IN list.
SourceNode* windowedSource(CorePlan& p)
{
    if (p.sources.size() != 1 || p.sources[0].kind != SourceNode::Kind::Table)
        return nullptr;
    SourceNode& s = p.sources[0];
    if (s.access.kind == AccessPath::Kind::Scan || s.access.inList || !s.onTerms.empty())
        return nullptr;
    return &s;
}

/// Drops an ORDER BY the access path already delivers: one table read
/// through its key or an index, ordered by the parts after the pinned ones
/// (pinned parts may appear anywhere: each holds one value), all ascending
/// and BINARY. Rows then arrive in order and the stable sort would leave them
/// as they are. All descending works the same over a backward walk, when the
/// terms reach the end of a unique key so that no two rows tie.
void elideOrder(SelectPlan& sp)
{
    if (sp.order.empty() || sp.cores.size() != 1)
        return;
    CorePlan& p = sp.cores[0];
    if (p.aggregate || !p.windows.empty() || p.distinct || !p.values.empty())
        return;
    SourceNode* s = windowedSource(p);
    if (!s)
        return;
    const std::vector<WalkedPart> parts = walkedParts(*s);
    const std::size_t pinned = s->access.equals.size();
    std::size_t next = pinned;
    int desc = -1;
    for (const SelectPlan::SortKey& k : sp.order) {
        const int col = walkedColumn(*s, p.outputs[k.column]);
        if (col < 0 || k.collation != Collation::Binary)
            return;
        bool constant = false;
        for (std::size_t i = 0; i < pinned && i < parts.size(); ++i)
            constant = constant || (parts[i].column == col && parts[i].plain);
        if (constant)
            continue;
        if (next >= parts.size() || parts[next].column != col || !parts[next].plain)
            return;
        // NULLs come first in key order: the default for ASC, last for DESC.
        if (k.nulls != 0 && k.nulls != (k.desc ? 2 : 1))
            return;
        if (desc >= 0 && desc != int(k.desc))
            return;
        desc = int(k.desc);
        ++next;
    }
    if (desc == 1) {
        if (s->access.kind == AccessPath::Kind::Index || next != parts.size())
            return;
        s->reverse = true;
    }
    sp.order.clear();
}

}  // namespace

void Planner::planFilters(CorePlan& core, Expr* where)
{
    planFiltersImpl(core, where);
    markCacheable(core);
}

void Planner::planFiltersImpl(CorePlan& core, Expr* where)
{
    const std::size_t n = core.sources.size();
    for (std::size_t d = 0; d < n; ++d)
        core.sources[d].written = d;
    std::vector<Expr*> whereTerms;
    conjuncts(where, whereTerms);

    ReadySet ready;
    ready.outer = core.slotBegin;

    if (reorderable(core)) {
        // Inner join: every ON term is just another WHERE term.
        std::vector<Expr*> pool = whereTerms;
        for (SourceNode& s : core.sources) {
            pool.insert(pool.end(), s.onTerms.begin(), s.onTerms.end());
            s.onTerms.clear();
        }
        // Which sources each term reads.
        std::vector<std::vector<bool>> reads(pool.size(), std::vector<bool>(n, false));
        std::vector<bool> readsAll(pool.size(), false);
        for (std::size_t k = 0; k < pool.size(); ++k) {
            bool all = false;
            sourcesRead(core, pool[k], reads[k], all);
            readsAll[k] = all;
        }
        // Greedy: next is the unplaced source with the narrowest access path
        // given what is placed, then the most terms it completes, then the
        // written order. A plan is never narrower than the predicate, so any
        // order is correct; this only decides how fast.
        std::vector<bool> placed(n, false);
        std::vector<std::size_t> order;
        for (std::size_t step = 0; step < n; ++step) {
            int best = -1;
            int bestRank = -1, bestTerms = -1;
            for (std::size_t d = 0; d < n; ++d) {
                if (placed[d])
                    continue;
                SourceNode& s = core.sources[d];
                int rank = 0;
                if (s.kind == SourceNode::Kind::Table && !s.notIndexed) {
                    planAccess(s, pool, ready);
                    rank = accessRank(s);
                }
                int completes = 0;
                for (std::size_t k = 0; k < pool.size(); ++k) {
                    if (readsAll[k] || !reads[k][d])
                        continue;
                    bool others = true;
                    for (std::size_t o = 0; o < n && others; ++o)
                        others = o == d || !reads[k][o] || placed[o];
                    if (others)
                        ++completes;
                }
                if (rank > bestRank || (rank == bestRank && completes > bestTerms)) {
                    best = int(d);
                    bestRank = rank;
                    bestTerms = completes;
                }
            }
            placed[std::size_t(best)] = true;
            order.push_back(std::size_t(best));
            const SourceNode& s = core.sources[std::size_t(best)];
            ready.ranges.emplace_back(s.base, s.base + s.width);
        }
        std::vector<SourceNode> sorted;
        sorted.reserve(n);
        for (const std::size_t d : order)
            sorted.push_back(std::move(core.sources[d]));
        core.sources = std::move(sorted);
        core.levelFilters.assign(n, {});
        for (std::size_t k = 0; k < pool.size(); ++k) {
            std::vector<bool> r(n, false);
            bool all = false;
            sourcesRead(core, pool[k], r, all);
            std::size_t level = 0;
            for (std::size_t d = 0; d < n; ++d) {
                if (r[d])
                    level = d;
            }
            if (all)
                level = n - 1;
            core.levelFilters[level].push_back(pool[k]);
        }
        ready.ranges.clear();
        for (std::size_t d = 0; d < n; ++d) {
            SourceNode& s = core.sources[d];
            if (s.kind == SourceNode::Kind::Table)
                planAccess(s, s.notIndexed ? std::vector<Expr*>() : pool, ready);
            ready.ranges.emplace_back(s.base, s.base + s.width);
        }
        return;
    }

    core.levelFilters.assign(n, {});
    if (core.hasRightJoin || n == 0) {
        core.finalFilters = whereTerms;
    } else {
        for (Expr* t : whereTerms) {
            std::vector<bool> r(n, false);
            bool all = false;
            sourcesRead(core, t, r, all);
            std::size_t level = 0;
            for (std::size_t d = 0; d < n; ++d) {
                if (r[d])
                    level = d;
            }
            if (all)
                level = n - 1;
            core.levelFilters[level].push_back(t);
        }
    }
    for (std::size_t d = 0; d < n; ++d) {
        SourceNode& s = core.sources[d];
        if (s.kind == SourceNode::Kind::Table) {
            std::vector<Expr*> avail = s.onTerms;
            const bool outer = s.join == JoinType::Left || s.join == JoinType::Full;
            if (!outer && !core.hasRightJoin)
                avail.insert(avail.end(), whereTerms.begin(), whereTerms.end());
            if (s.notIndexed)
                avail.clear();
            planAccess(s, avail, ready);
        }
        ready.ranges.emplace_back(s.base, s.base + s.width);
    }
}

// --------------------------------------------------------------- cores ---

std::string Planner::outputName(const Expr& e, const CorePlan& plan, std::string_view text, bool sub)
{
    if (e.kind == ExprKind::Column && e.slot >= 0) {
        if (sub)
            return e.name;
        for (const SourceNode& s : plan.sources) {
            if (std::size_t(e.slot) >= s.base && std::size_t(e.slot) < s.base + s.width) {
                const std::size_t k = std::size_t(e.slot) - s.base;
                if (k < s.columns.size())
                    return s.columns[k];
                return "rowid";
            }
        }
        return e.name;
    }
    if (e.end > e.begin && e.end <= text.size())
        return std::string(text.substr(e.begin, e.end - e.begin));
    return e.describe();
}

void Planner::expandStar(const ResultColumn& rc, CorePlan& plan, SelectPlan& sp, bool first)
{
    bool matched = false;
    for (SourceNode& s : plan.sources) {
        if (s.kind == SourceNode::Kind::Single)
            continue;
        if (!rc.starTable.empty() && !equalsNoCase(rc.starTable, s.alias))
            continue;
        matched = true;
        for (std::size_t k = 0; k < s.columns.size(); ++k) {
            if (s.hidden[k])
                continue;
            if (rc.starTable.empty()) {
                bool linked = false;
                for (const auto& link : s.usingLinks)
                    linked = linked || link.first == int(k);
                if (linked)
                    continue;
            }
            int rightSlot = -1;
            if (rc.starTable.empty() && rightLink(plan.sources, int(s.base + k), rightSlot)) {
                ExprPtr co = coalesceOf(int(s.base + k), rightSlot, s.columns[k]);
                plan.outputs.push_back(co.get());
                if (first) {
                    sp.names.push_back(s.columns[k]);
                    sp.subNames.push_back(s.columns[k]);
                    sp.qualified.push_back(s.alias + "." + s.columns[k]);
                }
                plan.synthetic.push_back(std::move(co));
                continue;
            }
            ExprPtr col = makeExpr(ExprKind::Column);
            col->slot = int(s.base + k);
            col->name = s.columns[k];
            col->table = s.alias;
            col->affinity = s.affinity[k];
            col->hasAffinity = true;
            col->collation = s.collation[k];
            col->collationStrength = 1;
            if (s.table && s.table->strict &&
                s.table->columns[k].strictType == StrictType::Datetime)
                col->strictDatetime = true;
            if (s.kind == SourceNode::Kind::Table)
                useColumn(s, k);
            plan.outputs.push_back(col.get());
            if (first) {
                sp.names.push_back(s.columns[k]);
                sp.subNames.push_back(s.columns[k]);
                sp.qualified.push_back(s.alias + "." + s.columns[k]);
            }
            plan.synthetic.push_back(std::move(col));
        }
    }
    if (!matched) {
        if (rc.starTable.empty())
            error("no tables specified");
        error("no such table: " + rc.starTable);
    }
}

void Planner::planCore(SelectCore& core, CorePlan& plan, Scope& scope, SelectPlan& sp, bool first,
                       std::vector<OrderTerm>* order, bool compound)
{
    plan.core = &core;
    plan.distinct = core.distinct;
    if (!core.values.empty()) {
        SourceNode single;
        single.kind = SourceNode::Kind::Single;
        plan.sources.push_back(std::move(single));
        plan.slotBegin = plan.slotEnd = prog_.frameSize;
        for (auto& row : core.values) {
            std::vector<Expr*> exprs;
            for (ExprPtr& e : row) {
                resolve(e, scope, 0);
                exprs.push_back(e.get());
            }
            plan.values.push_back(std::move(exprs));
        }
        plan.visible = core.values.front().size();
        for (Expr* e : plan.values.front())
            plan.outputs.push_back(e);
        if (first) {
            for (std::size_t i = 0; i < plan.visible; ++i) {
                sp.names.push_back("column" + std::to_string(i + 1));
                sp.subNames.push_back(sp.names.back());
                sp.qualified.emplace_back();
            }
        }
        plan.levelFilters.assign(1, {});
        plan.frameEnd = prog_.frameSize;
        return;
    }
    if (core.from.empty()) {
        SourceNode single;
        single.kind = SourceNode::Kind::Single;
        plan.sources.push_back(std::move(single));
        plan.slotBegin = plan.slotEnd = prog_.frameSize;
    } else {
        planFrom(core.from, plan, scope);
    }

    for (ResultColumn& rc : core.columns) {
        if (rc.star) {
            expandStar(rc, plan, sp, first);
            continue;
        }
        resolve(rc.expr, scope, kAllowAggregate | kAllowWindow);
        plan.outputs.push_back(rc.expr.get());
        if (first) {
            if (!rc.alias.empty()) {
                sp.names.push_back(rc.alias);
                sp.subNames.push_back(rc.alias);
            } else {
                sp.names.push_back(outputName(*rc.expr, plan, scope.text, false));
                sp.subNames.push_back(outputName(*rc.expr, plan, scope.text, true));
            }
            if (rc.expr->kind == ExprKind::Column) {
                std::string q;
                for (const SourceNode& s : plan.sources) {
                    if (std::size_t(rc.expr->slot) >= s.base &&
                        std::size_t(rc.expr->slot) < s.base + s.width)
                        q = s.alias + "." + rc.expr->name;
                }
                sp.qualified.push_back(q);
            } else {
                sp.qualified.emplace_back();
            }
        }
    }
    plan.visible = plan.outputs.size();

    if (core.where) {
        resolve(core.where, scope, kAllowAlias);
        if (containsKind(core.where.get(), ExprKind::Aggregate))
            error("misuse of aggregate function in WHERE clause");
    }
    for (ExprPtr& g : core.groupBy) {
        if (g->kind == ExprKind::Literal && g->literal.type() == Type::Integer) {
            const std::int64_t k = g->literal.integer();
            if (k < 1 || std::size_t(k) > plan.visible)
                error(ordinal(std::size_t(&g - core.groupBy.data()) + 1) +
                      " GROUP BY term out of range - should be between 1 and " +
                      std::to_string(plan.visible));
            const Expr& out = *plan.outputs[std::size_t(k - 1)];
            if (containsKind(&out, ExprKind::Aggregate))
                error("aggregate functions are not allowed in the GROUP BY clause");
            ExprPtr slot = makeExpr(ExprKind::Slot);
            // Point at the output expression by evaluating it: reuse via copy.
            if (out.kind == ExprKind::Column) {
                slot->kind = ExprKind::Column;
                slot->slot = out.slot;
                slot->affinity = out.affinity;
                slot->hasAffinity = out.hasAffinity;
                slot->collation = out.collation;
                slot->collationStrength = out.collationStrength;
                g = std::move(slot);
            } else {
                g = copyExpr(out, scope.text);
                resolve(g, scope, 0);
            }
        } else {
            resolve(g, scope, kAllowAlias);
        }
        if (containsKind(g.get(), ExprKind::Aggregate))
            error("aggregate functions are not allowed in the GROUP BY clause");
        plan.groupBy.push_back(g.get());
        plan.groupCollation.push_back(g->collationStrength ? g->collation : Collation::Binary);
    }
    if (core.having)
        resolve(core.having, scope, kAllowAggregate | kAllowAlias);

    if (order && !compound) {
        for (std::size_t i = 0; i < order->size(); ++i) {
            OrderTerm& t = (*order)[i];
            SelectPlan::SortKey key;
            key.desc = t.desc;
            key.nulls = t.nulls;
            Expr* e = t.expr.get();
            Collation explicitColl = Collation::Binary;
            bool hasColl = false;
            while (e->kind == ExprKind::Collate) {
                const auto c = collationOfName(e->name);
                if (!c)
                    error("no such collation sequence: " + e->name);
                if (!hasColl)
                    explicitColl = *c;
                hasColl = true;
                e = e->lhs.get();
            }
            int out = -1;
            if (e->kind == ExprKind::Literal && e->literal.type() == Type::Integer) {
                const std::int64_t k = e->literal.integer();
                if (k < 1 || std::size_t(k) > plan.visible)
                    error(ordinal(i + 1) + " ORDER BY term out of range - should be between 1 and " +
                          std::to_string(plan.visible));
                out = int(k - 1);
            } else if (e->kind == ExprKind::Column && e->table.empty()) {
                for (std::size_t k = 0; k < core.columns.size() && k < plan.visible; ++k) {
                    if (!core.columns[k].alias.empty() && equalsNoCase(core.columns[k].alias, e->name)) {
                        out = int(k);
                        break;
                    }
                }
            }
            if (out < 0) {
                resolve(t.expr, scope, kAllowAggregate | kAllowWindow);
                for (std::size_t k = 0; k < plan.visible; ++k) {
                    if (sameExpr(*plan.outputs[k], *t.expr)) {
                        out = int(k);
                        break;
                    }
                }
                if (out < 0) {
                    out = int(plan.outputs.size());
                    plan.outputs.push_back(t.expr.get());
                }
                if (t.expr->collationStrength) {
                    explicitColl = t.expr->collation;
                    hasColl = true;
                }
            }
            key.column = std::size_t(out);
            if (hasColl)
                key.collation = explicitColl;
            else if (plan.outputs[std::size_t(out)]->collationStrength)
                key.collation = plan.outputs[std::size_t(out)]->collation;
            sp.order.push_back(key);
        }
    }

    plan.aggregate = !plan.aggs.empty() || !plan.groupBy.empty();
    if (core.having && !plan.aggregate)
        error("a GROUP BY clause is required before HAVING");
    plan.having = core.having.get();
    plan.singleMinMax = plan.aggs.size() == 1 && plan.aggs[0].minMax;
    planFilters(plan, core.where.get());
    if (plan.distinct && !plan.aggregate && plan.windows.empty() && !core.where && plan.sources.size() == 1 &&
        plan.sources[0].kind == SourceNode::Kind::Table && plan.sources[0].onTerms.empty() &&
        plan.finalFilters.empty() && plan.outputs.size() == plan.visible) {
        SourceNode& s = plan.sources[0];
        bool ok = true;
        for (const auto& level : plan.levelFilters)
            ok = ok && level.empty();
        std::vector<int> cols;
        for (std::size_t i = 0; i < plan.visible && ok; ++i) {
            const Expr* o = plan.outputs[i];
            ok = o->kind == ExprKind::Column && std::size_t(o->slot) >= s.base &&
                 std::size_t(o->slot) < s.base + s.table->columns.size() &&
                 int(std::size_t(o->slot) - s.base) != s.table->rowidAlias;
            if (ok)
                cols.push_back(int(std::size_t(o->slot) - s.base));
        }
        if (ok)
            s.distinctColumns = std::move(cols);
    }
    if (plan.aggregate && plan.groupBy.empty() && plan.windows.empty() && !core.where &&
        plan.sources.size() == 1 && plan.sources[0].kind == SourceNode::Kind::Table &&
        plan.sources[0].onTerms.empty() && plan.finalFilters.empty()) {
        bool onlyCount = true;
        for (const AggInfo& a : plan.aggs)
            onlyCount = onlyCount && a.countStar && !a.call->filter && !a.call->distinct && a.call->orderBy.empty();
        for (const auto& level : plan.levelFilters)
            onlyCount = onlyCount && level.empty();
        // A bare column beside count(*) reads a row.
        const SourceNode& s = plan.sources[0];
        onlyCount = onlyCount && !s.needRowid && s.generated.empty();
        for (const std::uint8_t u : s.used)
            onlyCount = onlyCount && !u;
        plan.countRows = onlyCount;
    }
    if (plan.aggregate && plan.groupBy.empty() && plan.windows.empty() && core.where && !plan.countRows &&
        plan.sources.size() == 1 && plan.sources[0].kind == SourceNode::Kind::Table &&
        plan.sources[0].onTerms.empty() && plan.finalFilters.empty() && plan.sources[0].generated.empty()) {
        bool onlyCount = true;
        for (const AggInfo& a : plan.aggs)
            onlyCount = onlyCount && a.countStar && !a.call->filter && !a.call->distinct && a.call->orderBy.empty();
        // Nothing but the filters may read the row: no bare column beside
        // count(*), none in HAVING.
        const SourceNode& s = plan.sources[0];
        std::function<bool(const Expr*)> readsRow = [&](const Expr* e) {
            if (!e)
                return false;
            if (e->sub || e->select)
                return true;
            if ((e->kind == ExprKind::Column || e->kind == ExprKind::Slot) && e->slot >= 0 &&
                std::size_t(e->slot) >= s.base && std::size_t(e->slot) < s.base + s.width)
                return true;
            if (readsRow(e->lhs.get()) || readsRow(e->rhs.get()) || readsRow(e->extra.get()) ||
                readsRow(e->filter.get()))
                return true;
            for (const ExprPtr& x : e->list) {
                if (readsRow(x.get()))
                    return true;
            }
            return false;
        };
        for (const Expr* o : plan.outputs)
            onlyCount = onlyCount && !readsRow(o);
        onlyCount = onlyCount && !readsRow(plan.having);
        plan.countWindow = onlyCount;
    }
    if (plan.aggregate && plan.groupBy.empty() && plan.windows.empty() && plan.aggs.size() == 1 &&
        plan.aggs[0].minMax && plan.finalFilters.empty()) {
        const Expr& call = *plan.aggs[0].call;
        const SourceNode* s = windowedSource(plan);
        if (s && s->generated.empty() && !call.filter && call.orderBy.empty() &&
            call.collation == Collation::Binary) {
            const std::vector<WalkedPart> parts = walkedParts(*s);
            const std::size_t pinned = s->access.equals.size();
            const int col = walkedColumn(*s, call.list[0].get());
            // Nothing but the call may read the row: a bare column beside
            // min() or max() takes that row's value, and the seek reads one.
            std::function<bool(const Expr*)> readsRow = [&](const Expr* e) {
                if (!e || e == &call)
                    return false;
                if (e->sub || e->select)
                    return true;
                if ((e->kind == ExprKind::Column || e->kind == ExprKind::Slot) && e->slot >= 0 &&
                    std::size_t(e->slot) >= s->base && std::size_t(e->slot) < s->base + s->width)
                    return true;
                if (readsRow(e->lhs.get()) || readsRow(e->rhs.get()) || readsRow(e->extra.get()) ||
                    readsRow(e->filter.get()))
                    return true;
                for (const ExprPtr& x : e->list) {
                    if (readsRow(x.get()))
                        return true;
                }
                return false;
            };
            bool only = col >= 0 && pinned < parts.size() && parts[pinned].column == col &&
                        parts[pinned].plain && !parts[pinned].rowKey && !readsRow(plan.having);
            for (const Expr* o : plan.outputs)
                only = only && !readsRow(o);
            if (only)
                plan.extremeSeek = equalsNoCase(call.name, "max") ? 2 : 1;
        }
    }
    plan.frameEnd = prog_.frameSize;
}

std::unique_ptr<SelectPlan> Planner::planSelect(SelectStmt& s, Scope* outer, std::string_view text,
                                                SubqueryPlan* owner)
{
    auto sp = std::make_unique<SelectPlan>();
    Scope withScope;
    withScope.parent = outer;
    withScope.owner = owner;
    withScope.text = text;
    withScope.trigger = outer && outer->trigger;
    if (s.with) {
        for (CommonTableExpr& cte : s.with->ctes) {
            for (CteDef* other : withScope.ctes) {
                if (equalsNoCase(other->cte->name, cte.name))
                    error("duplicate WITH table name: " + cte.name);
            }
            auto d = std::make_unique<CteDef>();
            d->cte = &cte;
            d->text = text;
            d->recursive = s.with->recursive;
            d->home = &withScope;
            withScope.ctes.push_back(d.get());
            prog_.ctes.push_back(std::move(d));
        }
    }
    const bool compound = s.cores.size() > 1;
    sp->cores.resize(s.cores.size());
    for (std::size_t i = 0; i < s.cores.size(); ++i) {
        Scope scope;
        scope.parent = &withScope;
        scope.core = &sp->cores[i];
        scope.sources = &sp->cores[i].sources;
        scope.owner = owner;
        scope.text = text;
        scope.resultColumns = &s.cores[i].columns;
        scope.trigger = withScope.trigger;
        planCore(s.cores[i], sp->cores[i], scope, *sp, i == 0, &s.order, compound);
        if (i > 0 && sp->cores[i].visible != sp->cores[0].visible) {
            const char* op = "UNION";
            switch (s.ops[i - 1]) {
                case CompoundOp::UnionAll: op = "UNION ALL"; break;
                case CompoundOp::Union: op = "UNION"; break;
                case CompoundOp::Intersect: op = "INTERSECT"; break;
                case CompoundOp::Except: op = "EXCEPT"; break;
            }
            error(std::string("SELECTs to the left and right of ") + op +
                  " do not have the same number of result columns");
        }
    }
    sp->ops = s.ops;
    sp->columns = sp->cores[0].visible;
    for (std::size_t k = 0; k < sp->columns; ++k) {
        const Expr* e = sp->cores[0].outputs[k];
        sp->affinity.push_back(e->hasAffinity ? e->affinity : Affinity::Blob);
        sp->hasAffinity.push_back(e->hasAffinity);
        sp->collation.push_back(e->collation);
        sp->collationStrength.push_back(e->collationStrength);
        std::string decl;
        if (e->hasAffinity) {
            switch (e->affinity) {
                case Affinity::Integer: decl = "INT"; break;
                case Affinity::Real: decl = "REAL"; break;
                case Affinity::Text: decl = "TEXT"; break;
                case Affinity::Numeric: decl = "NUM"; break;
                case Affinity::Blob: break;
            }
        }
        sp->declTypes.push_back(decl);
    }
    if (compound && !s.order.empty()) {
        // A compound's ORDER BY can only name result columns.
        for (std::size_t i = 0; i < s.order.size(); ++i) {
            OrderTerm& t = s.order[i];
            SelectPlan::SortKey key;
            key.desc = t.desc;
            key.nulls = t.nulls;
            Expr* e = t.expr.get();
            bool hasColl = false;
            while (e->kind == ExprKind::Collate) {
                const auto c = collationOfName(e->name);
                if (!c)
                    error("no such collation sequence: " + e->name);
                if (!hasColl)
                    key.collation = *c;
                hasColl = true;
                e = e->lhs.get();
            }
            int out = -1;
            if (e->kind == ExprKind::Literal && e->literal.type() == Type::Integer) {
                const std::int64_t k = e->literal.integer();
                if (k < 1 || std::size_t(k) > sp->columns)
                    error(ordinal(i + 1) + " ORDER BY term out of range - should be between 1 and " +
                          std::to_string(sp->columns));
                out = int(k - 1);
            } else if (e->kind == ExprKind::Column && e->table.empty()) {
                for (std::size_t k = 0; k < sp->columns && out < 0; ++k) {
                    if (equalsNoCase(sp->names[k], e->name) || equalsNoCase(sp->subNames[k], e->name))
                        out = int(k);
                }
            }
            if (out < 0) {
                // An expression equal to a result column of the first core.
                const std::string want = std::string(text.substr(e->begin, e->end - e->begin));
                for (std::size_t k = 0; k < sp->columns && out < 0; ++k) {
                    const ResultColumn& rc = s.cores[0].columns[std::min(k, s.cores[0].columns.size() - 1)];
                    if (rc.expr && equalsNoCase(std::string(text.substr(rc.expr->begin, rc.expr->end - rc.expr->begin)), want))
                        out = int(k);
                }
            }
            if (out < 0)
                error(ordinal(i + 1) + " ORDER BY term does not match any column in the result set");
            key.column = std::size_t(out);
            if (!hasColl && sp->collationStrength[std::size_t(out)])
                key.collation = sp->collation[std::size_t(out)];
            sp->order.push_back(key);
        }
    }
    // Every core produces the same row width: visible plus hidden sort keys.
    std::size_t width = 0;
    for (const CorePlan& c : sp->cores)
        width = std::max(width, c.outputs.size());
    sp->rowWidth = width;
    elideOrder(*sp);
    if (s.limit) {
        resolve(s.limit, withScope, 0);
        sp->limit = s.limit.get();
    }
    if (s.offset) {
        resolve(s.offset, withScope, 0);
        sp->offset = s.offset.get();
    }
    if (s.forJson) {
        // Property paths: see docs/design.md, FOR JSON.
        const CorePlan& planned = sp->cores[0];
        // Nesting follows the FROM clause as written, whatever order the
        // joins run in.
        struct Written
        {
            std::vector<const SourceNode*> sources;
        } c0w;
        c0w.sources.resize(planned.sources.size());
        for (const SourceNode& src : planned.sources)
            c0w.sources[src.written < c0w.sources.size() ? src.written : 0] = &src;
        struct WrittenView
        {
            const std::vector<const SourceNode*>& v;
            std::size_t size() const { return v.size(); }
            const SourceNode& operator[](std::size_t i) const { return *v[i]; }
        };
        struct Core0
        {
            WrittenView sources;
        } c0{WrittenView{c0w.sources}};
        const bool autoMode = s.forJson->mode() == JsonMode::Auto;
        std::vector<std::string> prefix(c0.sources.size());
        for (std::size_t d = 1; d < c0.sources.size(); ++d)
            prefix[d] = prefix[d - 1] + c0.sources[d].alias + ".";
        std::size_t col = 0;
        for (const ResultColumn& rc : s.cores[0].columns) {
            if (rc.star) {
                for (std::size_t d = 0; d < c0.sources.size(); ++d) {
                    const SourceNode& src = c0.sources[d];
                    if (src.kind == SourceNode::Kind::Single || src.kind == SourceNode::Kind::Pseudo)
                        continue;
                    if (!rc.starTable.empty() && !equalsNoCase(rc.starTable, src.alias))
                        continue;
                    for (std::size_t k = 0; k < src.columns.size(); ++k) {
                        if (src.hidden[k])
                            continue;
                        if (rc.starTable.empty()) {
                            bool linked = false;
                            for (const auto& link : src.usingLinks)
                                linked = linked || link.first == int(k);
                            if (linked)
                                continue;
                        }
                        const std::string& name = src.columns[k];
                        const bool many = c0.sources.size() > 1;
                        sp->jsonPaths.push_back(autoMode ? prefix[d] + name
                                                         : (many ? src.alias + "." + name : name));
                        ++col;
                    }
                }
                continue;
            }
            const Expr& e = *rc.expr;
            const std::string property =
                (!rc.alias.empty()) ? rc.alias : (e.kind == ExprKind::Column ? e.name : sp->names[col]);
            if (autoMode) {
                int deep = 0;
                std::function<void(const Expr*)> walk = [&](const Expr* x) {
                    if (!x || x->kind == ExprKind::Aggregate)
                        return;
                    if (x->kind == ExprKind::Column) {
                        for (std::size_t d = 0; d < c0.sources.size(); ++d) {
                            const SourceNode& src = c0.sources[d];
                            if (std::size_t(x->slot) >= src.base && std::size_t(x->slot) < src.base + src.width)
                                deep = std::max(deep, int(d));
                        }
                    }
                    walk(x->lhs.get());
                    walk(x->rhs.get());
                    walk(x->extra.get());
                    for (const ExprPtr& y : x->list)
                        walk(y.get());
                };
                walk(&e);
                sp->jsonPaths.push_back(prefix[std::size_t(deep)] + property);
            } else {
                sp->jsonPaths.push_back(property);
            }
            ++col;
        }
    }
    if (s.forJson)
        presentAsJson(*sp, *s.forJson);
    return sp;
}

void presentAsJson(SelectPlan& sp, const JsonOptions& options)
{
    sp.forJson = options;
    sp.innerColumns = sp.columns;
    sp.innerNames = sp.names;
    sp.innerQualified = sp.qualified;
    sp.columns = 1;
    sp.names = {"json"};
    sp.subNames = {"json"};
    sp.qualified = {std::string()};
    sp.affinity = {Affinity::Blob};
    sp.hasAffinity = {false};
    sp.collation = {Collation::Binary};
    sp.collationStrength = {0};
    sp.declTypes = {std::string()};
}

void planSelectProgram(Program& p, Session& session)
{
    Planner planner(p, session);
    auto& s = std::get<SelectStmt>(p.node->body);
    p.select = planner.planSelect(s, nullptr, p.text, nullptr);
}

}  // namespace sql::internal
