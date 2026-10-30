// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Statement planning and execution: SELECT output (and FOR JSON), INSERT,
// UPDATE, DELETE with RETURNING and upsert, and dispatch of everything else.

#include <algorithm>
#include <set>

#include "internal/dml.hpp"
#include "internal/encoding.hpp"
#include "internal/parser.hpp"
#include "internal/stmt.hpp"
#include "internal/util.hpp"
#include "sql/json.hpp"

namespace sql::internal {

namespace {

/// Problems found while planning: the statement cannot run as written.
[[noreturn]] void error(const std::string& what)
{
    throw Error(ErrorCode::InvalidArgument, what);
}

/// Records which schema snapshots a plan depends on.
void pinSchemas(Program& p, Session& s)
{
    p.schemas.clear();
    p.schemaStamps.clear();
    for (std::size_t i = 0; i < s.stores.size(); ++i) {
        p.schemas.push_back(s.stores[i].catalog ? s.catalog(i).snapshot() : nullptr);
        p.schemaStamps.push_back(p.schemas.back() ? p.schemas.back()->stamp : 0);
    }
}

/// Finds the target table (or view) of a write.
const Table* targetTable(Planner& pl, const std::string& schema, const std::string& name,
                         std::size_t& store, const View** view)
{
    *view = nullptr;
    if (const Table* t = pl.findTable(schema, name, store))
        return t;
    Session& s = pl.session();
    for (std::size_t i = 0; i < s.stores.size(); ++i) {
        if (!s.stores[i].catalog)
            continue;
        if (!schema.empty() && !equalsNoCase(s.stores[i].name, schema))
            continue;
        if (const View* v = s.catalog(i).findView(name)) {
            *view = v;
            store = i;
            return nullptr;
        }
    }
    if (equalsNoCase(name, "sqlite_schema") || equalsNoCase(name, "sqlite_master"))
        throw Error(ErrorCode::ReadOnly, "table " + name + " may not be modified");
    throw Error(ErrorCode::NoSuchTable, "no such table: " + (schema.empty() ? "" : schema + ".") + name);
}

/// A view written through INSTEAD OF triggers is given a table-shaped
/// stand-in so the writer can carry NEW and OLD for it.
const Table* viewTable(Program& p, Planner& pl, const View& v, std::size_t store)
{
    auto t = std::make_shared<Table>();
    t->name = v.name;
    t->withoutRowid = false;
    // Column names from the view's select.
    Program probe;
    probe.text = v.sql;
    probe.node = std::make_unique<StatementNode>(parseOne(v.sql));
    auto& def = std::get<CreateViewStmt>(probe.node->body);
    Planner ppl(probe, pl.session());
    auto sp = ppl.planSelect(*def.select, nullptr, probe.text, nullptr);
    for (std::size_t i = 0; i < sp->columns; ++i) {
        Column c;
        c.name = def.columns.empty() ? sp->subNames[i] : def.columns[i];
        t->columns.push_back(c);
    }
    (void)store;
    p.ownedTables.push_back(t);
    return t.get();
}

/// A scope with one pseudo source: the table's row at `base`, under `alias`.
struct RowScope
{
    std::vector<SourceNode> sources;
    Scope scope;
};

void addPseudo(std::vector<SourceNode>& sources, const Table& t, std::size_t base, const std::string& alias)
{
    SourceNode s;
    s.kind = SourceNode::Kind::Pseudo;
    s.table = &t;
    s.name = t.name;
    s.alias = alias;
    for (const Column& c : t.columns) {
        s.columns.push_back(c.name);
        s.affinity.push_back(c.affinity);
        s.collation.push_back(c.collation);
        s.hidden.push_back(false);
    }
    s.base = base;
    s.width = t.width();
    sources.push_back(std::move(s));
}

/// RETURNING: expressions over the written row.
struct Returning
{
    std::vector<Expr*> exprs;
    std::vector<ExprPtr> owned;
    std::vector<std::string> names;
    std::vector<SourceNode> sources;
};

void planReturning(Planner& pl, std::vector<ResultColumn>& items, const Table& t, std::size_t base,
                   const std::string& alias, std::string_view text, Scope* outer, Returning& r)
{
    if (items.empty())
        return;
    addPseudo(r.sources, t, base, alias);
    Scope scope;
    scope.parent = outer;
    scope.sources = &r.sources;
    scope.text = text;
    for (ResultColumn& rc : items) {
        if (rc.star) {
            for (std::size_t k = 0; k < t.columns.size(); ++k) {
                ExprPtr col = makeExpr(ExprKind::Column);
                col->slot = int(base + k);
                col->name = t.columns[k].name;
                r.exprs.push_back(col.get());
                r.names.push_back(t.columns[k].name);
                r.owned.push_back(std::move(col));
            }
            continue;
        }
        pl.resolve(rc.expr, scope, 0);
        if (rc.expr->kind == ExprKind::Aggregate || rc.expr->kind == ExprKind::Window)
            error("aggregate functions are not allowed in RETURNING");
        r.exprs.push_back(rc.expr.get());
        if (!rc.alias.empty())
            r.names.push_back(rc.alias);
        else if (rc.expr->kind == ExprKind::Column)
            r.names.push_back(rc.expr->name);
        else
            r.names.push_back(std::string(text.substr(rc.expr->begin, rc.expr->end - rc.expr->begin)));
    }
}

void emitReturning(Returning& r, Ctx& c, ResultBuilder* out)
{
    if (r.exprs.empty() || !out)
        return;
    std::vector<Value> row;
    row.reserve(r.exprs.size());
    for (Expr* e : r.exprs)
        row.push_back(eval(*e, c));
    out->row(std::move(row));
}

void checkImmediateFks(Ctx& c)
{
    if (c.session->immediateFkViolations > 0) {
        c.session->immediateFkViolations = 0;
        throw Error(ErrorCode::ConstraintViolation, Constraint::ForeignKey, "FOREIGN KEY constraint failed");
    }
}

// --------------------------------------------------------------- INSERT --

struct InsertPlan final : ExecPlan
{
    Program* prog = nullptr;
    std::unique_ptr<Writer> writer;
    TablePlan* tp = nullptr;
    Conflict mode = Conflict::Default;
    std::vector<int> targets;  ///< column positions per source value (rowid = ncols)
    /// Per new-row slot: a value fills it, so initRow leaves its buffer alone.
    std::vector<std::uint8_t> targeted;
    std::vector<std::vector<Expr*>> values;  ///< direct VALUES rows
    SubqueryPlan* source = nullptr;          ///< INSERT ... SELECT
    bool defaultValues = false;
    std::vector<UpsertPlan> upserts;
    std::vector<Scope> upsertScopes;
    std::vector<SourceNode> upsertSources;
    Returning returning;
    std::vector<Value> scratchRow;

    bool insertOne(Ctx& c, const Value* v, std::size_t n, ResultBuilder* out, std::int64_t& changes)
    {
        writer->initRow(*tp, c, targeted.empty() ? nullptr : targeted.data());
        const Table& t = *tp->table;
        for (std::size_t i = 0; i < n; ++i) {
            const int col = targets[i];
            c.slots[tp->newBase + std::size_t(col)] = v[i];
            if (col == int(t.columns.size()) && t.rowidAlias >= 0)
                c.slots[tp->newBase + std::size_t(t.rowidAlias)] = v[i];
        }
        std::int64_t rowid = 0;
        writer->upserted = false;
        const bool done = writer->insert(*tp, c, mode, upserts.empty() ? nullptr : &upserts, rowid);
        if (done) {
            ++changes;
            if (t.hasRowid() && !tp->view)
                c.session->conn->lastInsertRowid = rowid;
            emitReturning(returning, c, out);
        } else if (writer->upserted) {
            ++changes;
            emitReturning(returning, c, out);
        }
        return true;
    }

    /// One row that cannot fail after its first write: every constraint is
    /// checked before the row and its index entries are written, so a
    /// failure leaves nothing behind and no nested transaction is needed.
    bool singleSafeRow(Ctx& c) const
    {
        const std::size_t rows = defaultValues ? 1 : (source ? 2 : values.size());
        return rows <= 1 && upserts.empty() && tp->triggers.empty() && !tp->view &&
               mode != Conflict::Replace && !c.session->conn->foreignKeys &&
               (!tp->table || [&] {
                   for (const IndexWriter& iw : tp->indexes) {
                       if (iw.index->unique && iw.index->onConflict == Conflict::Replace)
                           return false;
                   }
                   return tp->table->pkConflict != Conflict::Replace;
               }());
    }

    std::int64_t run(Ctx& c, ResultBuilder* out) override
    {
        std::int64_t changes = 0;
        c.session->immediateFkViolations = 0;
        if (!singleSafeRow(c))
            c.session->ensureAtomic();
        if (out && !returning.names.empty())
            out->columns(returning.names);
        if (defaultValues) {
            insertOne(c, nullptr, 0, out, changes);
        } else if (source) {
            runSubquery(*source, c);
            RowStore& rows = source->rows;
            // Rows were materialized first: a SELECT reading the target sees
            // none of this statement's inserts.
            std::vector<Value>& row = scratchRow;
            row.resize(rows.width());
            for (std::size_t r = 0; r < rows.size(); ++r) {
                const Value* v = rows.row(r);
                for (std::size_t i = 0; i < rows.width(); ++i)
                    row[i] = v[i];
                insertOne(c, row.data(), row.size(), out, changes);
            }
            source->cachedRun = 0;
        } else {
            std::vector<Value>& row = scratchRow;
            for (const auto& exprs : values) {
                row.resize(exprs.size());
                for (std::size_t i = 0; i < exprs.size(); ++i)
                    row[i] = eval(*exprs[i], c);
                insertOne(c, row.data(), row.size(), out, changes);
            }
        }
        checkImmediateFks(c);
        return changes;
    }
};

/// SQLite reserves sqlite_* names, but lets statements write the statistics
/// table ANALYZE keeps (and sqlite_sequence, which libsql keeps in its catalog).
bool userWritable(std::string_view name) noexcept
{
    return name.size() <= 7 || !equalsNoCase(name.substr(0, 7), "sqlite_") ||
           equalsNoCase(name, "sqlite_stat1") || equalsNoCase(name, "sqlite_sequence");
}

std::unique_ptr<ExecPlan> planInsert(Program& p, Session& s, Scope* outer)
{
    auto& st = std::get<InsertStmt>(p.node->body);
    auto plan = std::make_unique<InsertPlan>();
    plan->prog = &p;
    Planner pl(p, s);
    std::size_t store = 0;
    const View* view = nullptr;
    const Table* t = targetTable(pl, st.schema, st.table, store, &view);
    if (view)
        t = viewTable(p, pl, *view, store);
    if (!view && !userWritable(t->name))
        error("table " + t->name + " may not be modified");
    plan->writer = std::make_unique<Writer>(pl, p);
    plan->tp = &plan->writer->plan(*t, store);
    plan->tp->view = view != nullptr;
    if (view) {
        bool any = false;
        for (const TriggerPlan& tr : plan->tp->triggers)
            any = any || (tr.trigger->time == TriggerTime::InsteadOf && tr.trigger->event == TriggerEvent::Insert);
        if (!any)
            error("cannot modify " + t->name + " because it is a view");
    }
    plan->mode = st.orAction;
    const std::size_t ncols = t->columns.size();
    if (st.columns.empty()) {
        for (std::size_t i = 0; i < ncols; ++i) {
            if (!t->columns[i].generated)
                plan->targets.push_back(int(i));
        }
    } else {
        for (const std::string& name : st.columns) {
            const int pos = t->find(name);
            if (pos < 0)
                throw Error(ErrorCode::NoSuchColumn, "table " + t->name + " has no column named " + name);
            if (pos < int(ncols) && t->columns[std::size_t(pos)].generated)
                error("cannot INSERT into generated column \"" + name + "\"");
            plan->targets.push_back(pos);
        }
    }
    const std::string alias = st.alias.empty() ? t->name : st.alias;
    if (st.defaultValues) {
        plan->defaultValues = true;
        plan->targets.clear();
    } else {
        SelectStmt& sel = *st.select;
        const bool direct = sel.cores.size() == 1 && !sel.cores[0].values.empty() && !sel.with &&
                            sel.order.empty() && !sel.limit;
        if (direct) {
            Scope scope;
            scope.parent = outer;
            scope.text = p.text;
            scope.trigger = outer && outer->trigger;
            for (auto& row : sel.cores[0].values) {
                if (row.size() != plan->targets.size()) {
                    if (st.columns.empty())
                        error("table " + t->name + " has " + std::to_string(plan->targets.size()) +
                              " columns but " + std::to_string(row.size()) + " values were supplied");
                    error(std::to_string(row.size()) + " values for " + std::to_string(plan->targets.size()) +
                          " columns");
                }
                std::vector<Expr*> exprs;
                for (ExprPtr& e : row) {
                    pl.resolve(e, scope, 0);
                    exprs.push_back(e.get());
                }
                plan->values.push_back(std::move(exprs));
            }
        } else {
            SubqueryPlan* sub = pl.newSubquery(SubqueryPlan::Kind::Rows);
            sub->select = pl.planSelect(sel, outer, p.text, sub);
            sub->width = sub->select->columns;
            sub->correlated = true;  // never cached: each run inserts afresh
            if (sub->width != plan->targets.size()) {
                if (st.columns.empty())
                    error("table " + t->name + " has " + std::to_string(plan->targets.size()) +
                          " columns but " + std::to_string(sub->width) + " values were supplied");
                error(std::to_string(sub->width) + " values for " + std::to_string(plan->targets.size()) +
                      " columns");
            }
            plan->source = sub;
        }
    }
    // Upserts.
    if (!st.upserts.empty()) {
        if (view)
            error("cannot UPSERT a view");
        TablePlan& tp = *plan->tp;
        addPseudo(plan->upsertSources, *t, tp.oldBase, alias);
        addPseudo(plan->upsertSources, *t, tp.excludedBase, "excluded");
        plan->upsertSources.back().qualifiedOnly = true;
        plan->upsertScopes.resize(1);
        Scope& scope = plan->upsertScopes[0];
        scope.parent = outer;
        scope.sources = &plan->upsertSources;
        scope.text = p.text;
        for (Upsert& u : st.upserts) {
            UpsertPlan up;
            if (u.target.empty()) {
                up.any = true;
            } else {
                std::vector<int> cols;
                for (const IndexedColumn& ic : u.target) {
                    if (ic.expr->kind != ExprKind::Column)
                        error("ON CONFLICT clause does not match any PRIMARY KEY or UNIQUE constraint");
                    const int pos = t->find(ic.expr->name);
                    if (pos < 0)
                        throw Error(ErrorCode::NoSuchColumn, "no such column: " + ic.expr->name);
                    cols.push_back(pos == int(ncols) && t->rowidAlias >= 0 ? t->rowidAlias : pos);
                }
                std::vector<int> sorted = cols;
                std::sort(sorted.begin(), sorted.end());
                std::vector<int> pk = t->primaryKey;
                if (pk.empty() && t->hasRowid())
                    pk = {int(ncols)};
                std::sort(pk.begin(), pk.end());
                if (sorted == pk) {
                    up.primaryKey = true;
                } else {
                    for (const Index* ix : t->indexes) {
                        if (!ix->unique)
                            continue;
                        std::vector<int> ic;
                        for (const IndexPart& part : ix->parts)
                            ic.push_back(part.column);
                        std::sort(ic.begin(), ic.end());
                        if (ic == sorted && ix->whereSql.empty() == !u.targetWhere) {
                            up.index = ix;
                            break;
                        }
                    }
                    if (!up.index)
                        error("ON CONFLICT clause does not match any PRIMARY KEY or UNIQUE constraint");
                }
            }
            up.doNothing = u.doNothing;
            for (SetClause& sc : u.set) {
                pl.resolve(sc.value, scope, 0);
                if (sc.columns.size() != 1) {
                    error("row value misused");
                }
                const int pos = t->find(sc.columns[0]);
                if (pos < 0)
                    throw Error(ErrorCode::NoSuchColumn, "no such column: " + sc.columns[0]);
                up.set.emplace_back(pos, sc.value.get());
            }
            if (u.where) {
                pl.resolve(u.where, scope, 0);
                up.where = u.where.get();
            }
            plan->upserts.push_back(std::move(up));
        }
    }
    if (!plan->targets.empty()) {
        plan->targeted.assign(ncols + 1, 0);
        for (const int col : plan->targets) {
            plan->targeted[std::size_t(col)] = 1;
            if (col == int(ncols) && t->rowidAlias >= 0)
                plan->targeted[std::size_t(t->rowidAlias)] = 1;
        }
    }
    planReturning(pl, st.returning, *t, plan->tp->newBase, alias, p.text, outer, plan->returning);
    return plan;
}

// ------------------------------------------------------- UPDATE, DELETE ---

/// UPDATE and DELETE find their rows with an ordinary select over the target
/// (and UPDATE's FROM): its result is the target's old row (rowid last), then
/// UPDATE's new values. The rows are collected before anything is written.
struct ScanPlan
{
    std::unique_ptr<SelectStmt> select;
    std::unique_ptr<SelectPlan> plan;
    SubqueryPlan* sub = nullptr;
};

ScanPlan planScan(Program& p, Planner& pl, const Table& t, const std::string& alias, std::vector<FromItem>& from,
                  ExprPtr& where, std::vector<SetClause>* sets, std::vector<OrderTerm>& order, ExprPtr& limit,
                  ExprPtr& offset, Scope* outer, std::size_t store, const std::string& indexedBy, bool notIndexed,
                  std::unique_ptr<WithClause>& with)
{
    ScanPlan sp;
    sp.select = std::make_unique<SelectStmt>();
    // The statement's WITH: its tables are visible in WHERE, SET and FROM.
    sp.select->with = std::move(with);
    SelectCore core;
    FromItem target;
    target.kind = FromItem::Kind::Table;
    target.schema = pl.session().stores[store].name;
    target.name = t.name;
    target.alias = alias;
    target.indexedBy = indexedBy;
    target.notIndexed = notIndexed;
    core.from.push_back(std::move(target));
    for (FromItem& f : from) {
        FromItem moved = std::move(f);
        core.from.push_back(std::move(moved));
    }
    auto column = [&](const std::string& name) {
        ResultColumn rc;
        rc.expr = makeExpr(ExprKind::Column);
        rc.expr->table = alias;
        rc.expr->name = name;
        return rc;
    };
    for (const Column& c : t.columns)
        core.columns.push_back(column(c.name));
    if (t.hasRowid())
        core.columns.push_back(column("rowid"));
    if (sets) {
        for (SetClause& sc : *sets) {
            ResultColumn rc;
            rc.expr = std::move(sc.value);
            rc.begin = rc.expr->begin;
            rc.end = rc.expr->end;
            core.columns.push_back(std::move(rc));
        }
    }
    core.where = std::move(where);
    sp.select->cores.push_back(std::move(core));
    sp.select->order = std::move(order);
    sp.select->limit = std::move(limit);
    sp.select->offset = std::move(offset);
    sp.sub = pl.newSubquery(SubqueryPlan::Kind::Rows);
    sp.sub->correlated = true;
    sp.sub->select = pl.planSelect(*sp.select, outer, p.text, sp.sub);
    sp.sub->width = sp.sub->select->columns;
    // The target's columns must all be decoded.
    for (SourceNode& src : sp.sub->select->cores[0].sources) {
        if (src.table == &t)
            useAllColumns(src, t.columns.size());
    }
    return sp;
}

/// Single-row writes to a table without triggers, foreign keys or REPLACE
/// resolution cannot fail half-done (constraints are checked before the
/// first write), so they skip the statement's nested transaction.
bool safeSingleWrite(const TablePlan& tp, Conflict mode, std::size_t rows, Ctx& c)
{
    if (rows > 1 || !tp.triggers.empty() || tp.view || mode == Conflict::Replace ||
        c.session->conn->foreignKeys || tp.table->pkConflict == Conflict::Replace)
        return false;
    for (const IndexWriter& iw : tp.indexes) {
        if (iw.index->unique && iw.index->onConflict == Conflict::Replace)
            return false;
    }
    return true;
}

struct UpdatePlan final : ExecPlan
{
    std::unique_ptr<Writer> writer;
    TablePlan* tp = nullptr;
    Conflict mode = Conflict::Default;
    ScanPlan scan;
    std::vector<std::vector<int>> setColumns;  ///< per SET value: the columns it assigns
    std::vector<int> changed;
    Returning returning;
    bool view = false;
    bool from = false;  ///< UPDATE ... FROM: a target row may match several times
    std::set<std::string> done;
    std::string key;

    std::int64_t run(Ctx& c, ResultBuilder* out) override
    {
        std::int64_t changes = 0;
        c.session->immediateFkViolations = 0;
        if (out && !returning.names.empty())
            out->columns(returning.names);
        runSubquery(*scan.sub, c);
        // The matched rows are complete before anything is written. The
        // buffer is this plan's own: nothing the writes run can refill it.
        const RowStore& rows = scan.sub->rows;
        if (from || !safeSingleWrite(*tp, mode, rows.size(), c))
            c.session->ensureAtomic();
        const Table& t = *tp->table;
        const std::size_t w = t.width();
        done.clear();
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const Value* v = rows.row(r);
            for (std::size_t i = 0; i < w; ++i) {
                c.slots[tp->oldBase + i] = v[i];
                c.slots[tp->newBase + i] = v[i];
            }
            if (!view) {
                writer->storageKey(*tp, c, tp->oldBase, key);
                if (from && !done.insert(key).second)
                    continue;  // UPDATE FROM: each row at most once
                // A trigger or an earlier row may have changed this one.
                if (r > 0 || from) {
                    if (!writer->loadStored(*tp, c, key, tp->oldBase))
                        continue;
                    for (std::size_t i = 0; i < w; ++i)
                        c.slots[tp->newBase + i] = c.slots[tp->oldBase + i];
                }
            }
            std::size_t at = w;
            for (const std::vector<int>& cols : setColumns) {
                const Value& nv = v[at++];
                for (const int col : cols) {
                    c.slots[tp->newBase + std::size_t(col)] = nv;
                    if (!view && col == int(t.columns.size()) && t.rowidAlias >= 0)
                        c.slots[tp->newBase + std::size_t(t.rowidAlias)] = nv;
                    if (!view && col == t.rowidAlias)
                        c.slots[tp->newBase + t.columns.size()] = nv;
                }
            }
            if (writer->update(*tp, c, mode, key, &changed)) {
                ++changes;
                emitReturning(returning, c, out);
            }
        }
        checkImmediateFks(c);
        return changes;
    }
};

std::unique_ptr<ExecPlan> planUpdate(Program& p, Session& s, Scope* outer)
{
    auto& st = std::get<UpdateStmt>(p.node->body);
    auto plan = std::make_unique<UpdatePlan>();
    Planner pl(p, s);
    std::size_t store = 0;
    const View* view = nullptr;
    const Table* t = targetTable(pl, st.schema, st.table, store, &view);
    if (view) {
        error("cannot modify " + st.table + " because it is a view");
    }
    if (!userWritable(t->name))
        error("table " + t->name + " may not be modified");
    plan->writer = std::make_unique<Writer>(pl, p);
    plan->tp = &plan->writer->plan(*t, store);
    plan->mode = st.orAction;
    const std::string alias = st.alias.empty() ? t->name : st.alias;
    // Each SET target, checked before the values are moved into the scan.
    std::vector<SetClause> sets;
    for (SetClause& sc : st.set) {
        std::vector<int> cols;
        for (const std::string& name : sc.columns) {
            const int pos = t->find(name);
            if (pos < 0)
                throw Error(ErrorCode::NoSuchColumn, "no such column: " + name);
            if (pos < int(t->columns.size()) && t->columns[std::size_t(pos)].generated)
                error("cannot UPDATE generated column \"" + name + "\"");
            cols.push_back(pos);
            plan->changed.push_back(pos);
            if (pos == int(t->columns.size()) && t->rowidAlias >= 0)
                plan->changed.push_back(t->rowidAlias);
            if (pos == t->rowidAlias)
                plan->changed.push_back(int(t->columns.size()));
        }
        if (cols.size() > 1) {
            // (a, b) = (x, y) or (a, b) = (SELECT ...): one value per column.
            if (sc.value->kind == ExprKind::RowValue) {
                if (sc.value->list.size() != cols.size())
                    error(std::to_string(cols.size()) + " columns assigned " +
                          std::to_string(sc.value->list.size()) + " values");
                for (std::size_t k = 0; k < cols.size(); ++k) {
                    SetClause one;
                    one.columns.push_back(sc.columns[k]);
                    one.value = std::move(sc.value->list[k]);
                    plan->setColumns.push_back({cols[k]});
                    sets.push_back(std::move(one));
                }
                continue;
            }
            error("row value misused");
        }
        plan->setColumns.push_back(cols);
        sets.push_back(std::move(sc));
    }
    plan->from = !st.from.empty();
    plan->scan = planScan(p, pl, *t, alias, st.from, st.where, &sets, st.order, st.limit, st.offset, outer,
                          store, st.indexedBy, st.notIndexed, st.with);
    planReturning(pl, st.returning, *t, plan->tp->newBase, alias, p.text, outer, plan->returning);
    return plan;
}

/// A DELETE without triggers or RETURNING needs, of each row, its key, its
/// index columns and what the statement's terms read: the scan decodes only
/// those (a blob beside them is never copied). True when it narrowed the
/// scan; never with an index on an expression or a partial one, a generated
/// column or a term holding a sub-query.
bool keyColumns(const TablePlan& tp, SelectPlan& scan, bool noReturning)
{
    const Table& t = *tp.table;
    if (!noReturning || !tp.triggers.empty() || scan.cores.size() != 1)
        return false;
    CorePlan& core = scan.cores[0];
    if (core.sources.size() != 1 || core.sources[0].table != &t)
        return false;
    SourceNode& src = core.sources[0];
    std::vector<std::uint8_t> mask(t.columns.size(), 0);
    for (const Column& col : t.columns) {
        if (col.generated)
            return false;
    }
    for (const int col : t.primaryKey)
        mask[std::size_t(col)] = 1;
    for (const IndexWriter& iw : tp.indexes) {
        if (!iw.index->whereSql.empty())
            return false;
        for (const IndexPart& part : iw.index->parts) {
            if (part.column == -1)
                return false;
            if (part.column >= 0)
                mask[std::size_t(part.column)] = 1;
        }
    }
    for (const std::vector<Expr*>& level : core.levelFilters) {
        for (const Expr* f : level) {
            if (!columnsRead(f, src, mask))
                return false;
        }
    }
    for (const Expr* f : core.finalFilters) {
        if (!columnsRead(f, src, mask))
            return false;
    }
    // Hidden outputs are ORDER BY terms.
    for (std::size_t k = core.visible; k < core.outputs.size(); ++k) {
        if (!columnsRead(core.outputs[k], src, mask))
            return false;
    }
    src.used = std::move(mask);
    return true;
}

struct DeletePlan final : ExecPlan
{
    std::unique_ptr<Writer> writer;
    TablePlan* tp = nullptr;
    ScanPlan scan;
    Returning returning;
    std::string key;
    /// The scan decodes only what finds and removes a row (keyColumns).
    bool partialRows = false;

    std::int64_t run(Ctx& c, ResultBuilder* out) override
    {
        std::int64_t changes = 0;
        c.session->immediateFkViolations = 0;
        if (out && !returning.names.empty())
            out->columns(returning.names);
        runSubquery(*scan.sub, c);
        const RowStore& rows = scan.sub->rows;
        if (!safeSingleWrite(*tp, Conflict::Default, rows.size(), c))
            c.session->ensureAtomic();
        // Only a trigger or a foreign-key action can change a row this
        // statement has yet to reach; they also read whole rows.
        const bool reload = !tp->triggers.empty() || c.session->conn->foreignKeys;
        const std::size_t w = tp->table->width();
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const Value* v = rows.row(r);
            for (std::size_t i = 0; i < w; ++i)
                c.slots[tp->oldBase + i] = v[i];
            if (!tp->view) {
                writer->storageKey(*tp, c, tp->oldBase, key);
                if (reload && (r > 0 || partialRows) &&
                    !writer->loadStored(*tp, c, key, tp->oldBase))
                    continue;  // already gone (a trigger or cascade removed it)
            }
            if (writer->remove(*tp, c, key)) {
                ++changes;
                emitReturning(returning, c, out);
            }
        }
        checkImmediateFks(c);
        return changes;
    }
};

std::unique_ptr<ExecPlan> planDelete(Program& p, Session& s, Scope* outer)
{
    auto& st = std::get<DeleteStmt>(p.node->body);
    auto plan = std::make_unique<DeletePlan>();
    Planner pl(p, s);
    std::size_t store = 0;
    const View* view = nullptr;
    const Table* t = targetTable(pl, st.schema, st.table, store, &view);
    if (view)
        error("cannot modify " + st.table + " because it is a view");
    if (!userWritable(t->name))
        error("table " + t->name + " may not be modified");
    plan->writer = std::make_unique<Writer>(pl, p);
    plan->tp = &plan->writer->plan(*t, store);
    const std::string alias = st.alias.empty() ? t->name : st.alias;
    std::vector<FromItem> none;
    plan->scan = planScan(p, pl, *t, alias, none, st.where, nullptr, st.order, st.limit, st.offset, outer, store,
                          st.indexedBy, st.notIndexed, st.with);
    planReturning(pl, st.returning, *t, plan->tp->oldBase, alias, p.text, outer, plan->returning);
    plan->partialRows = keyColumns(*plan->tp, *plan->scan.sub->select, st.returning.empty());
    return plan;
}

// --------------------------------------------------------------- others --

struct PragmaPlan final : ExecPlan
{
    PragmaStmt* stmt = nullptr;
    std::int64_t run(Ctx& c, ResultBuilder* out) override
    {
        Value v;
        const Value* pv = nullptr;
        if (stmt->value) {
            v = eval(*stmt->value, c);
            pv = &v;
        }
        PragmaResult r = runPragma(*c.session, stmt->schema, stmt->name, pv, false);
        if (out && !r.columns.empty()) {
            out->columns(r.columns);
            for (auto& row : r.rows)
                out->row(std::move(row));
        }
        return 0;
    }
};

struct ExplainPlan final : ExecPlan
{
    std::unique_ptr<Program> inner;
    bool queryPlan = false;
    std::int64_t run(Ctx&, ResultBuilder* out) override
    {
        if (out)
            explainProgram(*inner, queryPlan, *out);
        return 0;
    }
};

/// Rows of a SELECT, into a ResultBuilder.
class ResultSink final : public RowSink
{
public:
    explicit ResultSink(ResultBuilder& out) : out_(out) {}
    bool row(Ctx&, const Value* v, std::size_t n) override
    {
        out_.row(v, n);
        return !out_.stopped;
    }
    bool take(Ctx&, Value* v, std::size_t n) override
    {
        out_.take(v, n);
        return !out_.stopped;
    }

private:
    ResultBuilder& out_;
};

/// Rows discarded (a SELECT inside a trigger body).
class NullSink final : public RowSink
{
public:
    bool row(Ctx&, const Value*, std::size_t) override { return true; }
};

}  // namespace

// --------------------------------------------------------------- entries --

void planStatement(Program& p, Session& s, Scope* outer)
{
    pinSchemas(p, s);
    StatementBody& body = p.node->body;
    if (auto* sel = std::get_if<SelectStmt>(&body)) {
        Planner pl(p, s);
        p.select = pl.planSelect(*sel, outer, p.text, nullptr);
    } else if (std::holds_alternative<InsertStmt>(body)) {
        p.exec = planInsert(p, s, outer);
    } else if (std::holds_alternative<UpdateStmt>(body)) {
        p.exec = planUpdate(p, s, outer);
    } else if (std::holds_alternative<DeleteStmt>(body)) {
        p.exec = planDelete(p, s, outer);
    } else if (auto* pr = std::get_if<PragmaStmt>(&body)) {
        auto plan = std::make_unique<PragmaPlan>();
        plan->stmt = pr;
        if (pr->value) {
            Planner pl(p, s);
            Scope scope;
            scope.text = p.text;
            pl.resolve(pr->value, scope, 0);
        }
        p.exec = std::move(plan);
    } else if (auto* ex = std::get_if<ExplainStmt>(&body)) {
        auto plan = std::make_unique<ExplainPlan>();
        plan->queryPlan = ex->queryPlan;
        plan->inner = std::make_unique<Program>();
        plan->inner->text = p.text;
        plan->inner->node = std::move(ex->statement);
        planStatement(*plan->inner, s, outer);
        p.exec = std::move(plan);
    } else if (std::holds_alternative<TransactionStmt>(body) || std::holds_alternative<AttachStmt>(body) ||
               std::holds_alternative<DetachStmt>(body) || std::holds_alternative<VacuumStmt>(body)) {
        throw Error(ErrorCode::Runtime, "cannot run this statement here");
    } else {
        p.exec = planDdl(p, s);
    }
    p.frame.resize(p.frameSize);
}

bool planCurrent(const Program& p, Session& s)
{
    if (!p.select && !p.exec)
        return false;
    if (p.schemas.size() != s.stores.size())
        return false;
    for (std::size_t i = 0; i < s.stores.size(); ++i) {
        const std::shared_ptr<const Schema> now = s.stores[i].catalog ? s.catalog(i).snapshot() : nullptr;
        if (p.schemas[i] != now || (now && p.schemaStamps[i] != now->stamp))
            return false;
    }
    return true;
}

namespace {

/// Drops the borrows a run left in the frame, so no later copy reads a page
/// of a transaction that has ended.
struct FrameGuard
{
    std::vector<Value>& frame;
    ~FrameGuard()
    {
        for (Value& v : frame) {
            if (v.borrowed())
                v.setNull();
        }
    }
};

/// Lets go of the store cursors the table sources hold once a run is over,
/// so a prepared statement does not keep its finished transaction out of
/// libnosql's pool until it runs again. Within the run they stay, so a
/// join's next probe lands near the last.
struct CursorGuard
{
    Program& p;
    ~CursorGuard()
    {
        auto release = [](SelectPlan* sp) {
            if (!sp)
                return;
            for (CorePlan& core : sp->cores) {
                for (SourceNode& s : core.sources) {
                    if (s.kind == SourceNode::Kind::Table)
                        closeSource(s);
                }
            }
        };
        release(p.select.get());
        for (const std::unique_ptr<SubqueryPlan>& sub : p.subqueries) {
            release(sub->select.get());
            release(sub->recursive.get());
        }
    }
};

}  // namespace

void runProgram(Program& p, Session& s, const std::vector<Value>& params, ResultBuilder& out, FuncContext& fctx)
{
    CursorGuard cursors{p};
    if (p.frame.size() < p.frameSize)
        p.frame.resize(p.frameSize);
    Ctx c;
    c.slots = p.frame.data();
    c.params = &params;
    c.session = &s;
    c.fctx = &fctx;
    c.run = ++s.conn->runCounter;
    fctx.nowUsec = INT64_MIN;
    fctx.conn = s.conn;
    if (p.select) {
        // A row callback may write through this transaction, and a write may
        // change pages in place.
        c.borrow = !((out.each || out.consumer) && s.writable);
        FrameGuard guard{p.frame};
        SelectPlan& sp = *p.select;
        if (!sp.sharedNames) {
            auto names = std::make_shared<ColumnNames>();
            names->names = sp.names;
            names->qualified = sp.qualified;
            sp.sharedNames = std::move(names);
        }
        out.declTypes = &sp.columnTypes;
        out.columns(sp.sharedNames);
        // Size the result like the last run's, so a statement that keeps
        // returning about as many rows does not regrow it row by row.
        out.reserve(sp.lastRows);
        ResultSink sink(out);
        runSelect(*p.select, c, sink);
        sp.lastRows = std::min<std::size_t>(out.rows(), 65536);
        return;
    }
    const std::int64_t changes = p.exec->run(c, &out);
    const StatementBody& body = p.node->body;
    if (std::holds_alternative<InsertStmt>(body) || std::holds_alternative<UpdateStmt>(body) ||
        std::holds_alternative<DeleteStmt>(body)) {
        s.conn->changes = changes;
        s.conn->totalChanges += changes;
        out.changed(std::uint64_t(changes));
        if (std::holds_alternative<InsertStmt>(body))
            out.lastInsertId(changes > 0 ? s.conn->lastInsertRowid.load() : 0);
    }
}

std::int64_t runNested(Program& p, Session& s, int depth, FuncContext& fctx)
{
    if (!planCurrent(p, s))
        planStatement(p, s, nullptr);
    if (p.frame.size() < p.frameSize)
        p.frame.resize(p.frameSize);
    Ctx c;
    c.slots = p.frame.data();
    static const std::vector<Value> kNoParams;
    c.params = &kNoParams;
    c.session = &s;
    c.fctx = &fctx;
    c.depth = depth;
    c.run = ++s.conn->runCounter;
    if (p.select) {
        c.borrow = true;
        FrameGuard guard{p.frame};
        NullSink sink;
        runSelect(*p.select, c, sink);
        return 0;
    }
    const std::int64_t n = p.exec->run(c, nullptr);
    s.conn->totalChanges += n;
    return n;
}

}  // namespace sql::internal
