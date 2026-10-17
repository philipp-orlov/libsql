// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Running a planned SELECT: nested-loop joins over the sources, then
// grouping, window functions, projection, DISTINCT, compound operators,
// ORDER BY and LIMIT -- each stage only when the statement asks for it, so a
// plain filtered scan runs as a single loop.

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <deque>
#include <limits>
#include <map>

#include "internal/encoding.hpp"
#include "internal/exec.hpp"
#include "internal/stmt.hpp"

namespace sql::internal {

namespace {

[[noreturn]] void error(const std::string& what)
{
    throw Error(ErrorCode::Runtime, what);
}

// ------------------------------------------------------------- sinks -----

class CollectSink final : public RowSink
{
public:
    explicit CollectSink(RowStore& store) : store_(store) {}
    bool row(Ctx&, const Value* v, std::size_t n) override
    {
        Value* out = store_.append();
        for (std::size_t i = 0; i < n && i < store_.width(); ++i)
            out[i] = v[i];
        return true;
    }

private:
    RowStore& store_;
};

/// Applies OFFSET and LIMIT to rows on their way to `next`.
class LimitSink final : public RowSink
{
public:
    LimitSink(RowSink& next, std::int64_t limit, std::int64_t offset)
        : next_(next), limit_(limit), offset_(offset)
    {}
    bool row(Ctx& c, const Value* v, std::size_t n) override
    {
        if (limit_ == 0)
            return false;
        if (offset_ > 0) {
            --offset_;
            return true;
        }
        if (!next_.row(c, v, n))
            return false;
        if (limit_ > 0 && --limit_ == 0)
            return false;
        return true;
    }
    bool take(Ctx& c, Value* v, std::size_t n) override
    {
        if (limit_ == 0)
            return false;
        if (offset_ > 0) {
            --offset_;
            return true;
        }
        if (!next_.take(c, v, n))
            return false;
        if (limit_ > 0 && --limit_ == 0)
            return false;
        return true;
    }

private:
    RowSink& next_;
    std::int64_t limit_;
    std::int64_t offset_;
};

/// Passes through the first `width` columns only.
class TrimSink final : public RowSink
{
public:
    TrimSink(RowSink& next, std::size_t width) : next_(next), width_(width) {}
    bool row(Ctx& c, const Value* v, std::size_t n) override
    {
        return next_.row(c, v, std::min(n, width_));
    }
    bool take(Ctx& c, Value* v, std::size_t n) override { return next_.take(c, v, std::min(n, width_)); }

private:
    RowSink& next_;
    std::size_t width_;
};

std::int64_t evalLimit(Expr* e, Ctx& c, const char* what)
{
    if (!e)
        return what[0] == 'L' ? -1 : 0;
    Value v = eval(*e, c);
    applyAffinity(v, Affinity::Numeric);
    if (v.type() != Type::Integer)
        error("datatype mismatch");
    std::int64_t n = v.integer();
    if (what[0] == 'L')
        return n < 0 ? -1 : n;
    return n < 0 ? 0 : n;
}

void appendRowKey(std::string& key, const Value* v, std::size_t n, const std::vector<Collation>* coll)
{
    for (std::size_t i = 0; i < n; ++i)
        appendKey(key, v[i], coll && i < coll->size() ? (*coll)[i] : Collation::Binary);
}

// -------------------------------------------------------------- joins ----

/// A conjunct the source's window already guarantees for this open.
inline bool enforced(const SourceNode& s, const Expr* f) noexcept
{
    for (const Expr* e : s.enforced) {
        if (e == f)
            return true;
    }
    return false;
}

[[noreturn]] void interrupted()
{
    throw Error(ErrorCode::Interrupted, "interrupted");
}

template <class Emit>
bool nest(CorePlan& p, Ctx& c, std::size_t d, Emit& emit)
{
    if (c.session && c.session->interrupt && c.session->interrupt->load(std::memory_order_relaxed))
        interrupted();
    if (d == p.sources.size()) {
        for (Expr* f : p.finalFilters) {
            if (!holds(f, c))
                return true;
        }
        return emit();
    }
    SourceNode& s = p.sources[d];
    if (s.cacheable) {
        if (s.cachedRun != c.run || c.run == 0) {
            // Fill: this source's rows, with its own terms applied.
            s.cachedRun = c.run;
            s.cache.reset(s.width);
            s.replaying = true;
            openSource(s, c);
            while (nextSource(s, c)) {
                bool ok = true;
                for (Expr* f : s.localFilters) {
                    if (!enforced(s, f) && !holds(f, c)) {
                        ok = false;
                        break;
                    }
                }
                if (!ok)
                    continue;
                if (s.cache.size() >= kSourceCacheRows) {
                    // Too big to keep: scan it each time from now on.
                    s.replaying = false;
                    break;
                }
                s.cache.appendRow(c.slots + s.base);
            }
            closeSource(s);
            if (!s.replaying) {
                s.cacheable = false;
                s.cache.reset(s.width);
                s.cache.shrink();
                return nest(p, c, d, emit);
            }
        }
        for (std::size_t r = 0; r < s.cache.size(); ++r) {
            const Value* row = s.cache.row(r);
            for (std::size_t i = 0; i < s.width; ++i)
                c.slots[s.base + i] = row[i];
            bool ok = true;
            for (Expr* f : s.joinFilters) {
                if (!holds(f, c)) {
                    ok = false;
                    break;
                }
            }
            if (ok && !nest(p, c, d + 1, emit))
                return false;
        }
        return true;
    }
    openSource(s, c);
    // Terms the window guarantees are not evaluated per row.
    const std::vector<Expr*>* levelPtr = &p.levelFilters[d];
    const std::vector<Expr*>* onPtr = &s.onTerms;
    if (!s.enforced.empty()) {
        s.liveFilters.clear();
        for (Expr* f : p.levelFilters[d]) {
            if (!enforced(s, f))
                s.liveFilters.push_back(f);
        }
        s.liveOn.clear();
        for (Expr* f : s.onTerms) {
            if (!enforced(s, f))
                s.liveOn.push_back(f);
        }
        levelPtr = &s.liveFilters;
        onPtr = &s.liveOn;
    }
    const std::vector<Expr*>& onTerms = *onPtr;
    bool matched = false;
    const bool outer = s.join == JoinType::Left || s.join == JoinType::Full;
    const bool trackRight = s.join == JoinType::Right || s.join == JoinType::Full;
    while (nextSource(s, c)) {
        bool ok = true;
        for (Expr* on : onTerms) {
            if (!holds(on, c)) {
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;
        matched = true;
        if (trackRight)
            s.matchedKeys.insert(s.rowKey);
        for (Expr* f : *levelPtr) {
            if (!holds(f, c)) {
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;
        if (!nest(p, c, d + 1, emit))
            return false;
    }
    if (!matched && outer) {
        nullSource(s, c);
        for (Expr* f : p.levelFilters[d]) {
            if (!holds(f, c))
                return true;
        }
        if (!nest(p, c, d + 1, emit))
            return false;
    }
    return true;
}

/// The unmatched rows of RIGHT/FULL joined sources: everything to their left
/// is NULL.
template <class Emit>
bool rightRemainder(CorePlan& p, Ctx& c, Emit& emit)
{
    for (std::size_t d = 0; d < p.sources.size(); ++d) {
        SourceNode& s = p.sources[d];
        if (s.join != JoinType::Right && s.join != JoinType::Full)
            continue;
        for (std::size_t l = 0; l < d; ++l)
            nullSource(p.sources[l], c);
        openSource(s, c);
        while (nextSource(s, c)) {
            if (s.matchedKeys.count(s.rowKey))
                continue;
            if (!nest(p, c, d + 1, emit))
                return false;
        }
    }
    return true;
}

template <class Emit>
bool runJoins(CorePlan& p, Ctx& c, Emit& emit)
{
    for (SourceNode& s : p.sources)
        s.matchedKeys.clear();
    if (!nest(p, c, 0, emit))
        return false;
    if (p.hasRightJoin)
        return rightRemainder(p, c, emit);
    return true;
}

// ---------------------------------------------------------- projection ---

bool project(CorePlan& p, Ctx& c, RowSink& sink)
{
    const std::size_t n = p.outputs.size();
    if (p.outRow.size() < n)
        p.outRow.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        p.outRow[i] = eval(*p.outputs[i], c);
    }
    if (p.distinct) {
        p.key.clear();
        for (std::size_t i = 0; i < p.visible; ++i)
            appendKey(p.key, p.outRow[i], p.outputs[i]->collationStrength ? p.outputs[i]->collation : Collation::Binary);
        if (!p.seen.insert(p.key).second)
            return true;
    }
    return sink.take(c, p.outRow.data(), n);
}

// ---------------------------------------------------------- aggregates ---

void snapshot(const CorePlan& p, const Ctx& c, std::vector<Value>& out)
{
    out.assign(c.slots + p.slotBegin, c.slots + p.frameEnd);
}

void restore(const CorePlan& p, Ctx& c, const std::vector<Value>& in)
{
    for (std::size_t i = 0; i < in.size(); ++i)
        c.slots[p.slotBegin + i] = in[i];
}

/// Readies `g` for a new group, reusing the states it already has.
void initGroup(CorePlan& p, Group& g)
{
    if (g.aggs.size() != p.aggs.size()) {
        g.aggs.clear();
        g.aggs.resize(p.aggs.size());
    }
    for (std::size_t i = 0; i < p.aggs.size(); ++i) {
        AggSlot& a = g.aggs[i];
        if (a.state)
            a.state->reset();
        else
            a.state = p.aggs[i].def->aggregate();
        if (p.aggs[i].call->distinct) {
            if (a.distinct)
                a.distinct->clear();
            else
                a.distinct = std::make_unique<std::set<std::string>>();
        }
        a.ordered.clear();
    }
}

/// The group for `key`, created (from the reusable pool) when new.
Group& findGroup(CorePlan& p, std::string_view key, bool& fresh)
{
    GroupTable& gt = p.groupTable;
    const auto it = gt.index.find(key);
    if (it != gt.index.end()) {
        fresh = false;
        return *gt.groups[it->second];
    }
    fresh = true;
    if (gt.used == gt.groups.size())
        gt.groups.push_back(std::make_unique<Group>());
    Group& g = *gt.groups[gt.used];
    g.key.assign(key.data(), key.size());
    initGroup(p, g);
    gt.index.emplace(std::string_view(g.key), gt.used);
    ++gt.used;
    return g;
}

int compareKeys(const std::vector<Value>& a, const std::vector<Value>& b, const std::vector<OrderTerm>& order)
{
    for (std::size_t i = 0; i < a.size(); ++i) {
        const OrderTerm& t = order[i];
        const Collation coll = t.expr->collationStrength ? t.expr->collation : Collation::Binary;
        int r;
        if (a[i].isNull() != b[i].isNull()) {
            const bool nullsFirst = t.nulls == 1 || (t.nulls == 0 && !t.desc);
            r = a[i].isNull() ? -1 : 1;
            if (!nullsFirst)
                r = -r;
            if (t.desc)
                r = -r;
        } else {
            r = compareValues(a[i], b[i], coll);
        }
        if (r != 0)
            return t.desc ? -r : r;
    }
    return 0;
}

void stepAggregates(CorePlan& p, Ctx& c, Group& g, bool& extremeChanged)
{
    extremeChanged = false;
    std::vector<const Value*>& argv = p.argv;
    for (std::size_t i = 0; i < p.aggs.size(); ++i) {
        const AggInfo& info = p.aggs[i];
        Expr& call = *info.call;
        AggSlot& slot = g.aggs[i];
        if (call.filter && !holds(call.filter.get(), c))
            continue;
        if (!call.orderBy.empty()) {
            OrderedStep st;
            for (const OrderTerm& t : call.orderBy)
                st.keys.push_back(eval(*t.expr, c));
            for (const ExprPtr& a : call.list)
                st.args.push_back(eval(*a, c));
            if (slot.distinct) {
                c.fctx->buf2.clear();
                appendKey(c.fctx->buf2, st.args[0], call.collation);
                if (st.args[0].isNull() || !slot.distinct->insert(c.fctx->buf2).second)
                    continue;
            }
            slot.ordered.push_back(std::move(st));
            continue;
        }
        argv.clear();
        for (const ExprPtr& a : call.list)
            argv.push_back(&eval(*a, c));
        if (slot.distinct) {
            if (argv[0]->isNull())
                continue;
            c.fctx->buf2.clear();
            appendKey(c.fctx->buf2, *argv[0], call.collation);
            if (!slot.distinct->insert(c.fctx->buf2).second)
                continue;
        }
        c.fctx->collation = call.collation;
        if (p.singleMinMax && !argv.empty() && !argv[0]->isNull()) {
            Value before;
            slot.state->value(*c.fctx, before);
            slot.state->step(*c.fctx, int(argv.size()), argv.data());
            Value after;
            slot.state->value(*c.fctx, after);
            if (before.isNull() || compareValues(before, after, call.collation) != 0)
                extremeChanged = true;
            continue;
        }
        slot.state->step(*c.fctx, int(argv.size()), argv.data());
    }
}

void finishAggregates(CorePlan& p, Ctx& c, Group& g)
{
    std::vector<const Value*>& argv = p.argv;
    for (std::size_t i = 0; i < p.aggs.size(); ++i) {
        const AggInfo& info = p.aggs[i];
        AggSlot& slot = g.aggs[i];
        c.fctx->collation = info.call->collation;
        if (!slot.ordered.empty()) {
            std::stable_sort(slot.ordered.begin(), slot.ordered.end(),
                             [&](const OrderedStep& a, const OrderedStep& b) {
                                 return compareKeys(a.keys, b.keys, info.call->orderBy) < 0;
                             });
            for (const OrderedStep& st : slot.ordered) {
                argv.clear();
                for (const Value& v : st.args)
                    argv.push_back(&v);
                slot.state->step(*c.fctx, int(argv.size()), argv.data());
            }
            slot.ordered.clear();
        }
        slot.state->value(*c.fctx, c.slots[info.call->slot]);
    }
}

// ------------------------------------------------------------- windows ---

class WindowRunner
{
public:
    WindowRunner(CorePlan& p, Ctx& c) : p_(p), c_(c) {}

    void add() { rows_.emplace_back(c_.slots + p_.slotBegin, c_.slots + p_.frameEnd); }

    bool finish(RowSink& sink)
    {
        const std::size_t n = rows_.size();
        results_.assign(p_.windows.size(), std::vector<Value>(n));
        std::vector<std::uint32_t> order(n);
        for (std::size_t si = 0; si < p_.windowSpecs.size(); ++si) {
            const WindowSpec& spec = *p_.windowSpecs[si];
            // Partition and order keys per row.
            std::vector<std::string> part(n);
            std::vector<std::vector<Value>> okeys(n);
            for (std::size_t r = 0; r < n; ++r) {
                load(r);
                for (const ExprPtr& e : spec.partition)
                    appendKey(part[r], eval(*e, c_), e->collationStrength ? e->collation : Collation::Binary);
                for (const OrderTerm& t : spec.order)
                    okeys[r].push_back(eval(*t.expr, c_));
            }
            for (std::size_t r = 0; r < n; ++r)
                order[r] = std::uint32_t(r);
            std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
                const int pc = part[a].compare(part[b]);
                if (pc != 0)
                    return pc < 0;
                return compareKeys(okeys[a], okeys[b], spec.order) < 0;
            });
            // Windows sharing PARTITION BY and ORDER BY share this sort, but
            // each keeps its own frame and EXCLUDE clause.
            for (std::size_t w = 0; w < p_.windows.size(); ++w) {
                if (p_.windows[w].specIndex != si)
                    continue;
                compute(w, *p_.windows[w].spec, order, part, okeys);
            }
            lastOrder_ = order;
        }
        if (p_.windowSpecs.empty()) {
            lastOrder_.resize(n);
            for (std::size_t r = 0; r < n; ++r)
                lastOrder_[r] = std::uint32_t(r);
        }
        for (const std::uint32_t r : lastOrder_) {
            load(r);
            for (std::size_t w = 0; w < p_.windows.size(); ++w)
                c_.slots[p_.windows[w].call->slot] = results_[w][r];
            if (!project(p_, c_, sink))
                return false;
        }
        return true;
    }

private:
    void load(std::size_t r)
    {
        const std::vector<Value>& v = rows_[r];
        for (std::size_t i = 0; i < v.size(); ++i)
            c_.slots[p_.slotBegin + i] = v[i];
    }

    std::int64_t offsetValue(const Expr* e, const char* what)
    {
        Value v = eval(*e, c_);
        applyAffinity(v, Affinity::Numeric);
        if ((v.type() != Type::Integer && v.type() != Type::Real) ||
            (v.type() == Type::Integer ? v.integer() < 0 : v.real() < 0))
            error(std::string("frame ") + what + " offset must be a non-negative number");
        return v.type() == Type::Integer ? v.integer() : std::int64_t(v.real());
    }

    void compute(std::size_t w, const WindowSpec& spec, const std::vector<std::uint32_t>& order,
                 const std::vector<std::string>& part, const std::vector<std::vector<Value>>& okeys)
    {
        const WinInfo& info = p_.windows[w];
        Expr& call = *info.call;
        const std::size_t n = order.size();
        // Arguments and filter per row (in sorted position order).
        std::vector<std::vector<Value>> args(n);
        std::vector<char> pass(n, 1);
        for (std::size_t i = 0; i < n; ++i) {
            load(order[i]);
            for (const ExprPtr& a : call.list)
                args[i].push_back(eval(*a, c_));
            if (call.filter)
                pass[i] = holds(call.filter.get(), c_);
        }
        auto peer = [&](std::size_t a, std::size_t b) {
            return compareKeys(okeys[order[a]], okeys[order[b]], spec.order) == 0;
        };
        std::size_t ps = 0;
        while (ps < n) {
            std::size_t pe = ps + 1;
            while (pe < n && part[order[pe]] == part[order[ps]])
                ++pe;
            // Peer groups inside the partition.
            std::vector<std::size_t> groupStart(pe - ps), groupEnd(pe - ps), groupNo(pe - ps);
            {
                std::size_t g = ps, no = 0;
                while (g < pe) {
                    std::size_t ge = g + 1;
                    while (ge < pe && peer(g, ge))
                        ++ge;
                    for (std::size_t k = g; k < ge; ++k) {
                        groupStart[k - ps] = g;
                        groupEnd[k - ps] = ge;
                        groupNo[k - ps] = no;
                    }
                    ++no;
                    g = ge;
                }
            }
            if (info.builtin)
                builtin(info, spec, order, args, ps, pe, groupStart, groupEnd, groupNo, okeys, results_[w]);
            else
                aggregate(info, spec, order, args, pass, ps, pe, groupStart, groupEnd, groupNo, okeys,
                          results_[w]);
            ps = pe;
        }
    }

    /// Frame [start, end) of row i (sorted positions), partition [ps, pe).
    void frame(const WindowSpec& spec, std::size_t i, std::size_t ps, std::size_t pe,
               const std::vector<std::size_t>& gs, const std::vector<std::size_t>& ge,
               const std::vector<std::size_t>& gn, const std::vector<std::uint32_t>& order,
               const std::vector<std::vector<Value>>& okeys, std::size_t& start, std::size_t& end)
    {
        if (!spec.hasFrame) {
            start = ps;
            end = spec.order.empty() ? pe : ge[i - ps];
            return;
        }
        auto bound = [&](FrameBound kind, const Expr* e, bool isStart) -> std::size_t {
            switch (kind) {
                case FrameBound::UnboundedPreceding: return ps;
                case FrameBound::UnboundedFollowing: return pe;
                case FrameBound::CurrentRow:
                    if (spec.unit == FrameUnit::Rows)
                        return isStart ? i : i + 1;
                    return isStart ? gs[i - ps] : ge[i - ps];
                case FrameBound::Preceding:
                case FrameBound::Following: {
                    const bool preceding = kind == FrameBound::Preceding;
                    if (spec.unit == FrameUnit::Rows) {
                        const std::int64_t off = offsetValue(e, isStart ? "starting" : "ending");
                        std::int64_t pos = std::int64_t(i) + (preceding ? -off : off);
                        if (!isStart)
                            ++pos;
                        pos = std::max<std::int64_t>(std::int64_t(ps), std::min<std::int64_t>(pos, std::int64_t(pe)));
                        return std::size_t(pos);
                    }
                    if (spec.unit == FrameUnit::Groups) {
                        const std::int64_t off = offsetValue(e, isStart ? "starting" : "ending");
                        const std::int64_t target = std::int64_t(gn[i - ps]) + (preceding ? -off : off);
                        // Find the peer group `target`.
                        if (target < 0)
                            return isStart ? ps : ps;
                        for (std::size_t k = ps; k < pe; ++k) {
                            if (std::int64_t(gn[k - ps]) == target)
                                return isStart ? gs[k - ps] : ge[k - ps];
                        }
                        return pe;
                    }
                    // RANGE: by the value of the single ORDER BY key.
                    const OrderTerm& t = spec.order[0];
                    const Value& cur = okeys[order[i]][0];
                    if (cur.isNull())
                        return isStart ? gs[i - ps] : ge[i - ps];
                    Value offv = eval(*e, c_);
                    applyAffinity(offv, Affinity::Numeric);
                    if (offv.type() != Type::Integer && offv.type() != Type::Real)
                        error("frame starting offset must be a non-negative number");
                    double off = offv.type() == Type::Integer ? double(offv.integer()) : offv.real();
                    if (off < 0)
                        error("frame starting offset must be a non-negative number");
                    double c0 = toDouble(cur);
                    const bool towardStart = preceding != t.desc;
                    const double limit = towardStart ? c0 - off : c0 + off;
                    // rows ordered by key (desc flips); start: first row whose
                    // key is not before `limit`; end: past the last not after it.
                    std::size_t k = ps;
                    if (isStart) {
                        for (k = ps; k < pe; ++k) {
                            const Value& v = okeys[order[k]][0];
                            if (v.isNull())
                                continue;
                            const double x = toDouble(v);
                            if (t.desc ? x <= limit : x >= limit)
                                break;
                        }
                        return k;
                    }
                    for (k = pe; k > ps; --k) {
                        const Value& v = okeys[order[k - 1]][0];
                        if (v.isNull()) {
                            continue;
                        }
                        const double x = toDouble(v);
                        if (t.desc ? x >= limit : x <= limit)
                            break;
                    }
                    return k;
                }
            }
            return ps;
        };
        start = bound(spec.startKind, spec.startExpr.get(), true);
        end = bound(spec.endKind, spec.endExpr.get(), false);
        if (end < start)
            end = start;
    }

    bool excluded(const WindowSpec& spec, std::size_t i, std::size_t k, std::size_t ps,
                  const std::vector<std::size_t>& gs)
    {
        switch (spec.exclude) {
            case FrameExclude::NoOthers: return false;
            case FrameExclude::CurrentRow: return k == i;
            case FrameExclude::Group: return gs[k - ps] == gs[i - ps];
            case FrameExclude::Ties: return k != i && gs[k - ps] == gs[i - ps];
        }
        return false;
    }

    void aggregate(const WinInfo& info, const WindowSpec& spec, const std::vector<std::uint32_t>& order,
                   const std::vector<std::vector<Value>>& args, const std::vector<char>& pass,
                   std::size_t ps, std::size_t pe, const std::vector<std::size_t>& gs,
                   const std::vector<std::size_t>& ge, const std::vector<std::size_t>& gn,
                   const std::vector<std::vector<Value>>& okeys, std::vector<Value>& out)
    {
        Expr& call = *info.call;
        if (!state_ || stateFor_ != info.def) {
            state_ = info.def->aggregate();
            stateFor_ = info.def;
        }
        state_->reset();
        c_.fctx->collation = call.collation;
        const bool invertible = (info.def->flags & kInvertible) != 0;
        std::vector<const Value*> argv;
        auto apply = [&](std::size_t k, bool add) {
            if (!pass[k])
                return;
            argv.clear();
            for (const Value& v : args[k])
                argv.push_back(&v);
            if (add)
                state_->step(*c_.fctx, int(argv.size()), argv.data());
            else
                state_->inverse(*c_.fctx, int(argv.size()), argv.data());
        };
        std::size_t curStart = ps, curEnd = ps;
        for (std::size_t i = ps; i < pe; ++i) {
            std::size_t start, end;
            frame(spec, i, ps, pe, gs, ge, gn, order, okeys, start, end);
            const bool slide = spec.exclude == FrameExclude::NoOthers && start >= curStart &&
                               end >= curEnd && (start == curStart || invertible);
            if (slide) {
                for (std::size_t k = curEnd; k < end; ++k)
                    apply(k, true);
                for (std::size_t k = curStart; k < start && k < curEnd; ++k)
                    apply(k, false);
                curStart = start;
                curEnd = std::max(end, curEnd);
            } else {
                state_->reset();
                for (std::size_t k = start; k < end; ++k) {
                    if (!excluded(spec, i, k, ps, gs))
                        apply(k, true);
                }
                curStart = start;
                curEnd = end;
                if (spec.exclude != FrameExclude::NoOthers) {
                    // The state now excludes rows; force a recompute next time.
                    curStart = pe;
                    curEnd = pe;
                }
            }
            state_->value(*c_.fctx, out[order[i]]);
        }
    }

    void builtin(const WinInfo& info, const WindowSpec& spec, const std::vector<std::uint32_t>& order,
                 const std::vector<std::vector<Value>>& args, std::size_t ps, std::size_t pe,
                 const std::vector<std::size_t>& gs, const std::vector<std::size_t>& ge,
                 const std::vector<std::size_t>& gn, const std::vector<std::vector<Value>>& okeys,
                 std::vector<Value>& out)
    {
        const std::size_t count = pe - ps;
        for (std::size_t i = ps; i < pe; ++i) {
            Value& r = out[order[i]];
            switch (info.builtin) {
                case 1: r.setInteger(std::int64_t(i - ps + 1)); break;  // row_number
                case 2: r.setInteger(std::int64_t(gs[i - ps] - ps + 1)); break;  // rank
                case 3: r.setInteger(std::int64_t(gn[i - ps] + 1)); break;  // dense_rank
                case 4:  // percent_rank
                    r.setReal(count > 1 ? double(gs[i - ps] - ps) / double(count - 1) : 0.0);
                    break;
                case 5: r.setReal(double(ge[i - ps] - ps) / double(count)); break;  // cume_dist
                case 6: {  // ntile
                    const Value& nv = args[i][0];
                    Value conv = nv;
                    applyAffinity(conv, Affinity::Numeric);
                    if (conv.type() != Type::Integer || conv.integer() <= 0)
                        error("argument of ntile must be a positive integer");
                    const std::int64_t buckets = conv.integer();
                    const std::int64_t k = std::int64_t(i - ps);
                    const std::int64_t size = std::int64_t(count) / buckets;
                    const std::int64_t large = std::int64_t(count) % buckets;
                    std::int64_t b;
                    if (size == 0)
                        b = k + 1;
                    else if (k < large * (size + 1))
                        b = k / (size + 1) + 1;
                    else
                        b = (k - large * (size + 1)) / size + large + 1;
                    r.setInteger(b);
                    break;
                }
                case 7:    // lag
                case 8: {  // lead
                    std::int64_t off = 1;
                    if (args[i].size() > 1) {
                        if (args[i][1].isNull()) {
                            r.setNull();
                            break;
                        }
                        off = toInt64(args[i][1]);
                    }
                    const std::int64_t at = std::int64_t(i) + (info.builtin == 7 ? -off : off);
                    if (at >= std::int64_t(ps) && at < std::int64_t(pe))
                        r = args[std::size_t(at)][0];
                    else if (args[i].size() > 2)
                        r = args[i][2];
                    else
                        r.setNull();
                    break;
                }
                case 9:    // first_value
                case 10:   // last_value
                case 11: { // nth_value
                    std::size_t start, end;
                    frame(spec, i, ps, pe, gs, ge, gn, order, okeys, start, end);
                    std::vector<std::size_t> rows;
                    for (std::size_t k = start; k < end; ++k) {
                        if (!excluded(spec, i, k, ps, gs))
                            rows.push_back(k);
                    }
                    if (info.builtin == 11) {
                        Value nv = args[i][1];
                        applyAffinity(nv, Affinity::Numeric);
                        if (nv.type() != Type::Integer || nv.integer() <= 0)
                            error("second argument to nth_value must be a positive integer");
                        const std::size_t nth = std::size_t(nv.integer());
                        if (nth <= rows.size())
                            r = args[rows[nth - 1]][0];
                        else
                            r.setNull();
                    } else if (rows.empty()) {
                        r.setNull();
                    } else {
                        r = args[info.builtin == 9 ? rows.front() : rows.back()][0];
                    }
                    break;
                }
                default: r.setNull(); break;
            }
        }
    }

    CorePlan& p_;
    Ctx& c_;
    std::vector<std::vector<Value>> rows_;
    std::vector<std::vector<Value>> results_;
    std::vector<std::uint32_t> lastOrder_;
    std::unique_ptr<AggregateState> state_;
    const FunctionDef* stateFor_ = nullptr;
};

// --------------------------------------------------------------- cores ---

/// Runs one core, delivering rows of `p.outputs.size()` values.
bool runCore(CorePlan& p, Ctx& c, RowSink& sink)
{
    p.seen.clear();
    if (!p.values.empty()) {
        if (p.outRow.size() < p.visible)
            p.outRow.resize(p.visible);
        for (auto& row : p.values) {
            for (std::size_t i = 0; i < row.size(); ++i)
                p.outRow[i] = eval(*row[i], c);
            if (!sink.row(c, p.outRow.data(), row.size()))
                return false;
        }
        return true;
    }
    const bool windowed = !p.windows.empty();
    if (!p.aggregate) {
        if (!windowed) {
            auto emit = [&] { return project(p, c, sink); };
            return runJoins(p, c, emit);
        }
        WindowRunner wr(p, c);
        auto emit = [&] {
            wr.add();
            return true;
        };
        runJoins(p, c, emit);
        return wr.finish(sink);
    }

    GroupTable& gt = p.groupTable;
    gt.used = 0;
    gt.index.clear();
    Group* only = nullptr;  // without GROUP BY every row lands in one group
    auto emit = [&] {
        bool fresh = false;
        Group* gp = only;
        if (!gp) {
            p.key.clear();
            for (std::size_t i = 0; i < p.groupBy.size(); ++i)
                appendKey(p.key, eval(*p.groupBy[i], c), p.groupCollation[i]);
            gp = &findGroup(p, p.key, fresh);
            if (p.groupBy.empty())
                only = gp;
        }
        Group& g = *gp;
        bool changed = false;
        stepAggregates(p, c, g, changed);
        if (fresh || changed)
            snapshot(p, c, g.sample);
        return true;
    };
    std::int64_t counted = 0;
    if (p.countRows) {
        const SourceNode& s = p.sources[0];
        counted = std::int64_t(c.session->catalog(s.schemaIndex).rows(*s.table).count());
    } else {
        runJoins(p, c, emit);
    }
    if (gt.used == 0 && p.groupBy.empty()) {
        bool fresh = false;
        Group& g = findGroup(p, std::string_view(), fresh);
        for (std::size_t i = p.slotBegin; i < p.frameEnd; ++i)
            c.slots[i].setNull();
        snapshot(p, c, g.sample);
    }
    // Groups come out in key order, as SQLite's sorter delivers them.
    gt.order.resize(gt.used);
    for (std::size_t i = 0; i < gt.used; ++i)
        gt.order[i] = std::uint32_t(i);
    std::sort(gt.order.begin(), gt.order.end(),
              [&](std::uint32_t a, std::uint32_t b) { return gt.groups[a]->key < gt.groups[b]->key; });
    WindowRunner wr(p, c);
    bool more = true;
    for (const std::uint32_t gi : gt.order) {
        Group& g = *gt.groups[gi];
        restore(p, c, g.sample);
        finishAggregates(p, c, g);
        if (p.countRows) {
            for (const AggInfo& a : p.aggs)
                c.slots[a.call->slot].setInteger(counted);
        }
        if (p.having && !holds(p.having, c))
            continue;
        if (windowed) {
            wr.add();
            continue;
        }
        if (!project(p, c, sink)) {
            more = false;
            break;
        }
    }
    // One unusually large result should not pin its groups for ever.
    if (gt.groups.size() > 4 * gt.used + 64) {
        gt.groups.resize(gt.used);
        gt.index = std::unordered_map<std::string_view, std::size_t>();
    }
    if (!more)
        return false;
    if (windowed)
        return wr.finish(sink);
    return true;
}

// ------------------------------------------------------------ sorting ----

/// ORDER BY order between two produced rows: <0, 0 or >0.
int compareSortRows(const SelectPlan& sp, const Value* x, const Value* y)
{
    for (const SelectPlan::SortKey& k : sp.order) {
        const Value& u = x[k.column];
        const Value& v = y[k.column];
        if (u.isNull() != v.isNull()) {
            const bool nullsFirst = k.nulls == 1 || (k.nulls == 0 && !k.desc);
            return u.isNull() ? (nullsFirst ? -1 : 1) : (nullsFirst ? 1 : -1);
        }
        const int r = compareValues(u, v, k.collation);
        if (r != 0)
            return k.desc ? -r : r;
    }
    return 0;
}

void sortRows(SelectPlan& sp, std::size_t limit)
{
    RowStore& rows = sp.sorted;
    std::vector<std::uint32_t>& idx = sp.orderIndex;
    idx.resize(rows.size());
    for (std::size_t i = 0; i < idx.size(); ++i)
        idx[i] = std::uint32_t(i);
    // Ties keep arrival order, so the sort is stable.
    auto less = [&](std::uint32_t a, std::uint32_t b) {
        const int r = compareSortRows(sp, rows.row(a), rows.row(b));
        return r != 0 ? r < 0 : a < b;
    };
    if (limit < idx.size())
        std::partial_sort(idx.begin(), idx.begin() + std::ptrdiff_t(limit), idx.end(), less);
    else
        std::sort(idx.begin(), idx.end(), less);
}

/// Cells a plan's sort buffer may keep between runs (about 3 MiB of Values);
/// a larger run gives its memory back when it is done.
constexpr std::size_t kKeepSortCells = 1u << 16;

/// A temporary file that is gone when closed, holding sorted runs.
struct TempFile
{
    std::FILE* f = nullptr;
    TempFile() = default;
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    ~TempFile()
    {
        if (f)
            std::fclose(f);
    }
    void seek(std::uint64_t at)
    {
#ifdef _WIN32
        const int rc = _fseeki64(f, static_cast<__int64>(at), SEEK_SET);
#else
        const int rc = fseeko(f, static_cast<off_t>(at), SEEK_SET);
#endif
        if (rc != 0)
            throw Error(ErrorCode::Runtime, "disk I/O error while sorting");
    }
};

/// ORDER BY's collector. With LIMIT it keeps only the rows that can still
/// make the cut (pruning to them whenever it holds twice as many); without,
/// once its rows pass the connection's sort memory it writes them as a
/// sorted run to a temporary file, and emit() merges the runs. Ties keep
/// arrival order throughout, so the output is that of one stable sort.
class SortCollector final : public RowSink
{
public:
    SortCollector(SelectPlan& sp, Ctx& c, std::int64_t keep) : sp_(sp), rows_(sp.sorted), keep_(keep)
    {
        rows_.reset(sp.rowWidth);
        const ConnectionState* conn = c.session ? c.session->conn : nullptr;
        if (conn) {
            spill_ = conn->tempStore.load(std::memory_order_relaxed) != 2;
            limit_ = conn->sortMemory.load(std::memory_order_relaxed);
        }
        if (keep_ >= 0)
            pruneAt_ = std::max<std::size_t>(std::size_t(keep_) * 2, 1024);
    }

    bool row(Ctx&, const Value* v, std::size_t n) override
    {
        if (cannotPlace(v))
            return true;
        Value* out = rows_.append();
        for (std::size_t i = 0; i < n && i < rows_.width(); ++i)
            out[i] = v[i];
        added(out);
        return true;
    }
    bool take(Ctx&, Value* v, std::size_t n) override
    {
        if (cannotPlace(v))
            return true;
        Value* out = rows_.append();
        for (std::size_t i = 0; i < n && i < rows_.width(); ++i)
            out[i] = std::move(v[i]);
        added(out);
        return true;
    }

    /// Hands rows [offset, want) of the sorted whole to `sink`; `want` < 0
    /// means all of them.
    void emit(Ctx& c, RowSink& sink, std::size_t offset, std::int64_t limit, std::size_t columns)
    {
        const std::uint64_t total = spilled_ + rows_.size();
        const std::uint64_t want = limit < 0 ? total : std::min<std::uint64_t>(total, std::uint64_t(limit) + offset);
        if (runs_.empty()) {
            sortRows(sp_, std::size_t(want));
            for (std::size_t i = offset; i < want; ++i) {
                if (!sink.take(c, rows_.row(sp_.orderIndex[i]), columns))
                    break;
            }
            return;
        }
        sortRows(sp_, rows_.size());
        merge(c, sink, offset, want, columns);
    }

private:
    static std::size_t bytesOf(const Value* row, std::size_t width) noexcept
    {
        std::size_t b = width * sizeof(Value);
        for (std::size_t i = 0; i < width; ++i) {
            if (row[i].type() == Type::Text)
                b += row[i].text().size();
            else if (row[i].type() == Type::Blob)
                b += row[i].blob().size();
        }
        return b;
    }

    /// With LIMIT, once pruned: a row no better than the worst one kept
    /// (a tie loses to the earlier arrival) can never make the cut.
    bool cannotPlace(const Value* v) const
    {
        return pruned_ && compareSortRows(sp_, v, rows_.row(std::size_t(keep_) - 1)) >= 0;
    }

    void added(const Value* row)
    {
        if (keep_ >= 0) {
            if (rows_.size() >= pruneAt_)
                prune();
            return;
        }
        bytes_ += bytesOf(row, rows_.width());
        if (spill_ && bytes_ > limit_ && rows_.size() > 1)
            writeRun();
    }

    /// Keeps the `keep_` best rows, in order.
    void prune()
    {
        sortRows(sp_, std::size_t(keep_));
        const std::size_t width = rows_.width();
        scratch_.reset(width);
        for (std::size_t i = 0; i < std::size_t(keep_); ++i) {
            Value* src = rows_.row(sp_.orderIndex[i]);
            Value* dst = scratch_.append();
            for (std::size_t k = 0; k < width; ++k)
                dst[k] = std::move(src[k]);
        }
        rows_.reset(width);
        for (std::size_t i = 0; i < scratch_.size(); ++i) {
            Value* src = scratch_.row(i);
            Value* dst = rows_.append();
            for (std::size_t k = 0; k < width; ++k)
                dst[k] = std::move(src[k]);
        }
        // Rows 0..keep_-1 are now the best so far, in order.
        pruned_ = rows_.size() == std::size_t(keep_) && keep_ > 0;
    }

    /// Sorts what is in memory and appends it to the file as one run.
    void writeRun()
    {
        if (!file_.f) {
            file_.f = std::tmpfile();
            if (!file_.f)
                throw Error(ErrorCode::Runtime, "unable to open a temporary file for sorting");
        }
        sortRows(sp_, rows_.size());
        const std::size_t width = rows_.width();
        Run run;
        run.begin = fileEnd_;
        run.rows = rows_.size();
        file_.seek(fileEnd_);
        std::string record;
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const Value* row = rows_.row(sp_.orderIndex[i]);
            body_.clear();
            for (std::size_t k = 0; k < width; ++k) {
                body_.push_back(char(row[k].subtype()));
                putValue(body_, row[k]);
            }
            record.clear();
            putVarint(record, body_.size());
            record += body_;
            if (std::fwrite(record.data(), 1, record.size(), file_.f) != record.size())
                throw Error(ErrorCode::Runtime, "disk I/O error while sorting (is the temp directory full?)");
            fileEnd_ += record.size();
        }
        run.end = fileEnd_;
        runs_.push_back(run);
        spilled_ += rows_.size();
        rows_.reset(width);
        bytes_ = 0;
    }

    struct Run
    {
        std::uint64_t begin = 0, end = 0;
        std::size_t rows = 0;
    };

    /// Reads a run back row by row through a buffer of its own.
    struct Reader
    {
        std::uint64_t pos = 0, end = 0;
        std::string buf;
        std::size_t at = 0;
        std::vector<Value> row;
        bool done = false;
    };

    bool fill(Reader& r, std::size_t need)
    {
        if (r.buf.size() - r.at >= need)
            return true;
        r.buf.erase(0, r.at);
        r.at = 0;
        const std::uint64_t left = r.end - r.pos;
        if (left == 0)
            return r.buf.size() >= need;
        const std::size_t chunk = std::size_t(std::min<std::uint64_t>(left, std::max<std::size_t>(need, 64u << 10)));
        const std::size_t have = r.buf.size();
        r.buf.resize(have + chunk);
        file_.seek(r.pos);
        if (std::fread(&r.buf[have], 1, chunk, file_.f) != chunk)
            throw Error(ErrorCode::Runtime, "disk I/O error while sorting");
        r.pos += chunk;
        return r.buf.size() >= need;
    }

    void advance(Reader& r, std::size_t width)
    {
        if (r.at == r.buf.size() && r.pos == r.end) {
            r.done = true;
            return;
        }
        fill(r, 10);
        std::string_view head(r.buf.data() + r.at, r.buf.size() - r.at);
        const std::size_t before = head.size();
        const std::uint64_t len = getVarint(head);
        const std::size_t prefix = before - head.size();
        if (!fill(r, prefix + std::size_t(len)))
            throw Error(ErrorCode::Internal, "truncated sort run");
        std::string_view body(r.buf.data() + r.at + prefix, std::size_t(len));
        r.row.resize(width);
        for (std::size_t k = 0; k < width; ++k) {
            const std::uint8_t sub = std::uint8_t(body.front());
            body.remove_prefix(1);
            getValueInto(body, r.row[k]);
            r.row[k].setSubtype(sub);
        }
        r.at += prefix + std::size_t(len);
    }

    void merge(Ctx& c, RowSink& sink, std::uint64_t offset, std::uint64_t want, std::size_t columns)
    {
        const std::size_t width = rows_.width();
        // Sources: the runs in the order they were written, then the rows
        // still in memory -- the order rows arrived in, for ties.
        std::vector<Reader> readers(runs_.size());
        for (std::size_t i = 0; i < runs_.size(); ++i) {
            readers[i].pos = runs_[i].begin;
            readers[i].end = runs_[i].end;
            advance(readers[i], width);
        }
        const std::size_t memory = runs_.size();
        std::size_t memAt = 0;
        auto current = [&](std::size_t s) -> Value* {
            return s == memory ? rows_.row(sp_.orderIndex[memAt]) : readers[s].row.data();
        };
        auto exhausted = [&](std::size_t s) {
            return s == memory ? memAt >= rows_.size() : readers[s].done;
        };
        auto later = [&](std::size_t a, std::size_t b) {
            const int r = compareSortRows(sp_, current(a), current(b));
            return r != 0 ? r > 0 : a > b;
        };
        std::vector<std::size_t> heap;
        for (std::size_t s = 0; s <= memory; ++s) {
            if (!exhausted(s))
                heap.push_back(s);
        }
        std::make_heap(heap.begin(), heap.end(), later);
        for (std::uint64_t n = 0; n < want && !heap.empty(); ++n) {
            std::pop_heap(heap.begin(), heap.end(), later);
            const std::size_t s = heap.back();
            if (n >= offset && !sink.take(c, current(s), columns))
                return;
            if (s == memory)
                ++memAt;
            else
                advance(readers[s], width);
            if (exhausted(s))
                heap.pop_back();
            else
                std::push_heap(heap.begin(), heap.end(), later);
        }
    }

    SelectPlan& sp_;
    RowStore& rows_;
    RowStore scratch_;
    std::int64_t keep_;
    std::size_t pruneAt_ = 0;
    bool pruned_ = false;
    bool spill_ = false;
    std::uint64_t limit_ = 64ull << 20;
    std::uint64_t bytes_ = 0;
    std::uint64_t spilled_ = 0;
    TempFile file_;
    std::uint64_t fileEnd_ = 0;
    std::vector<Run> runs_;
    std::string body_;
};

}  // namespace

namespace {

void runRows(SelectPlan& sp, Ctx& c, RowSink& sink);

/// Collects rows into a Result for the JSON writer.
class JsonCollect final : public RowSink
{
public:
    explicit JsonCollect(ResultBuilder& out) : out_(out) {}
    bool row(Ctx&, const Value* v, std::size_t n) override
    {
        out_.row(v, n);
        return true;
    }

private:
    ResultBuilder& out_;
};

}  // namespace

void runSelect(SelectPlan& sp, Ctx& c, RowSink& sink)
{
    if (!sp.forJson) {
        runRows(sp, c, sink);
        return;
    }
    ResultBuilder rows;
    rows.columns(sp.innerNames, sp.innerQualified);
    JsonCollect collect(rows);
    runRows(sp, c, collect);
    static thread_local JsonWriter writer;
    writer.options(*sp.forJson);
    Value doc{std::string(writer.write(rows.result, sp.jsonPaths))};
    doc.setSubtype(kJsonSubtype);
    sink.row(c, &doc, 1);
}

namespace {

void runRows(SelectPlan& sp, Ctx& c, RowSink& sink)
{
    const std::size_t columns = sp.forJson ? sp.innerColumns : sp.columns;
    const std::int64_t limit = evalLimit(sp.limit, c, "LIMIT");
    const std::int64_t offset = evalLimit(sp.offset, c, "OFFSET");
    if (limit == 0)
        return;
    const bool compound = sp.cores.size() > 1;

    if (!compound && sp.order.empty()) {
        TrimSink trim(sink, columns);
        if (limit < 0 && offset == 0) {
            runCore(sp.cores[0], c, trim);
            return;
        }
        LimitSink lim(trim, limit, offset);
        runCore(sp.cores[0], c, lim);
        return;
    }

    if (!compound) {
        // ORDER BY over one core: bounded by LIMIT, spilled when large.
        const std::int64_t keep =
            limit < 0 || limit > std::numeric_limits<std::int64_t>::max() - offset ? -1 : limit + offset;
        SortCollector collect(sp, c, keep);
        runCore(sp.cores[0], c, collect);
        collect.emit(c, sink, std::size_t(offset), limit, columns);
        sp.sorted.releaseAbove(kKeepSortCells);
        return;
    }
    RowStore& rows = sp.sorted;
    rows.reset(columns);
    {
        // Left to right: UNION ALL appends; the others work on keyed sets.
        std::vector<Collation> coll = sp.collation;
        CollectSink collect(rows);
        runCore(sp.cores[0], c, collect);
        RowStore right;
        std::string key;
        bool distinct = false;
        for (std::size_t i = 0; i < sp.ops.size(); ++i) {
            const CompoundOp op = sp.ops[i];
            right.reset(columns);
            CollectSink rs(right);
            runCore(sp.cores[i + 1], c, rs);
            if (op == CompoundOp::UnionAll) {
                for (std::size_t r = 0; r < right.size(); ++r)
                    rows.appendRow(right.row(r));
                distinct = false;
                continue;
            }
            // Distinct left side.
            std::map<std::string, std::size_t> left;
            RowStore merged;
            merged.reset(columns);
            for (std::size_t r = 0; r < rows.size(); ++r) {
                key.clear();
                appendRowKey(key, rows.row(r), columns, &coll);
                if (left.emplace(key, merged.size()).second)
                    merged.appendRow(rows.row(r));
            }
            std::set<std::string> rkeys;
            for (std::size_t r = 0; r < right.size(); ++r) {
                key.clear();
                appendRowKey(key, right.row(r), columns, &coll);
                if (op == CompoundOp::Union) {
                    if (left.emplace(key, merged.size()).second)
                        merged.appendRow(right.row(r));
                } else {
                    rkeys.insert(key);
                }
            }
            // Rebuild `rows` in key order, as SQLite's compound results come out.
            RowStore next;
            next.reset(columns);
            for (const auto& [k, at] : left) {
                const bool inRight = rkeys.count(k) != 0;
                if (op == CompoundOp::Intersect && !inRight)
                    continue;
                if (op == CompoundOp::Except && inRight)
                    continue;
                next.appendRow(merged.row(at));
            }
            rows.reset(columns);
            for (std::size_t r = 0; r < next.size(); ++r)
                rows.appendRow(next.row(r));
            distinct = true;
        }
        (void)distinct;
    }

    const std::size_t want =
        limit < 0 ? rows.size() : std::size_t(std::min<std::int64_t>(std::int64_t(rows.size()), limit + offset));
    if (!sp.order.empty()) {
        sortRows(sp, want);
    } else {
        sp.orderIndex.resize(rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i)
            sp.orderIndex[i] = std::uint32_t(i);
    }
    for (std::size_t i = std::size_t(offset); i < want; ++i) {
        if (!sink.take(c, rows.row(sp.orderIndex[i]), columns))
            break;
    }
    rows.releaseAbove(kKeepSortCells);
}

}  // namespace

// --------------------------------------------------------- sub-queries ---

namespace {

class FirstRowSink final : public RowSink
{
public:
    explicit FirstRowSink(SubqueryPlan& s) : s_(s) {}
    bool row(Ctx&, const Value* v, std::size_t n) override
    {
        s_.exists = true;
        if (s_.kind == SubqueryPlan::Kind::Scalar) {
            s_.scalar = v[0];
            s_.rows.reset(n);
            s_.rows.appendRow(v);
        }
        return false;
    }

private:
    SubqueryPlan& s_;
};

class KeySink final : public RowSink
{
public:
    explicit KeySink(SubqueryPlan& s) : s_(s) {}
    bool row(Ctx&, const Value* v, std::size_t n) override
    {
        key_.clear();
        for (std::size_t i = 0; i < n; ++i) {
            if (v[i].isNull()) {
                s_.hasNull = true;
                return true;
            }
            if (s_.rowAffinity[i] != Affinity::Blob) {
                Value conv = v[i];
                applyComparisonAffinity(conv, s_.rowAffinity[i]);
                appendKey(key_, conv, s_.keyCollation[i]);
            } else {
                appendKey(key_, v[i], s_.keyCollation[i]);
            }
        }
        s_.keys.insert(key_);
        return true;
    }

private:
    SubqueryPlan& s_;
    std::string key_;
};

void runRecursive(SubqueryPlan& s, Ctx& c)
{
    const std::size_t w = s.width;
    s.rows.reset(w);
    std::int64_t limit = evalLimit(s.limit, c, "LIMIT");
    std::int64_t offset = evalLimit(s.offset, c, "OFFSET");
    std::set<std::string> seen;
    std::string key;
    // The queue: FIFO, or ordered by the CTE's ORDER BY.
    std::deque<std::vector<Value>> fifo;
    auto cmp = [&](const std::vector<Value>& a, const std::vector<Value>& b) {
        for (std::size_t i = 0; i < s.queueOrder.size(); ++i) {
            const int r = compareValues(a[s.queueOrder[i]], b[s.queueOrder[i]]);
            if (r != 0)
                return s.queueDesc[i] ? r > 0 : r < 0;
        }
        return false;
    };
    std::multiset<std::vector<Value>, decltype(cmp)> ordered(cmp);
    const bool useOrdered = !s.queueOrder.empty();
    class Push final : public RowSink
    {
    public:
        std::function<void(const Value*)> fn;
        bool row(Ctx&, const Value* v, std::size_t) override
        {
            fn(v);
            return true;
        }
    } push;
    push.fn = [&](const Value* v) {
        if (!s.recursiveUnionAll) {
            key.clear();
            appendRowKey(key, v, w, nullptr);
            if (!seen.insert(key).second)
                return;
        }
        std::vector<Value> row(v, v + w);
        if (useOrdered)
            ordered.insert(std::move(row));
        else
            fifo.push_back(std::move(row));
    };
    runSelect(*s.select, c, push);
    std::vector<Value> current;
    while (useOrdered ? !ordered.empty() : !fifo.empty()) {
        if (useOrdered) {
            current = *ordered.begin();
            ordered.erase(ordered.begin());
        } else {
            current = std::move(fifo.front());
            fifo.pop_front();
        }
        if (offset > 0) {
            --offset;
        } else {
            s.rows.appendRow(current.data());
            if (limit > 0 && std::int64_t(s.rows.size()) >= limit)
                break;
        }
        s.current = current.data();
        runSelect(*s.recursive, c, push);
        s.current = nullptr;
    }
    s.current = nullptr;
}

}  // namespace

void runSubquery(SubqueryPlan& s, Ctx& c)
{
    if (!s.correlated && s.cachedRun == c.run && c.run != 0)
        return;
    s.cachedRun = c.run;
    switch (s.kind) {
        case SubqueryPlan::Kind::Scalar:
        case SubqueryPlan::Kind::Exists: {
            s.exists = false;
            s.scalar.setNull();
            s.rows.reset(s.width);
            FirstRowSink sink(s);
            runSelect(*s.select, c, sink);
            return;
        }
        case SubqueryPlan::Kind::In: {
            s.keys.clear();
            s.hasNull = false;
            KeySink sink(s);
            runSelect(*s.select, c, sink);
            return;
        }
        case SubqueryPlan::Kind::Rows: {
            if (s.generator) {
                s.rows.reset(s.width);
                s.generator(c, s.rows);
                return;
            }
            if (s.recursive) {
                runRecursive(s, c);
                return;
            }
            s.rows.reset(s.width);
            CollectSink sink(s.rows);
            runSelect(*s.select, c, sink);
            return;
        }
    }
}

}  // namespace sql::internal
