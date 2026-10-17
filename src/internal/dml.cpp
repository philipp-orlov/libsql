// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Row writes with SQLite's constraint semantics. The order of work for an
// INSERT mirrors SQLite's: defaults and affinity, BEFORE triggers, rowid,
// generated columns, NOT NULL, CHECK, PRIMARY KEY and UNIQUE (with the
// conflict algorithm or upsert in force), the write itself, foreign keys,
// AFTER triggers.

#include "internal/dml.hpp"

#include <algorithm>
#include <cmath>

#include "internal/encoding.hpp"
#include "internal/parser.hpp"
#include "internal/stmt.hpp"
#include "internal/util.hpp"

namespace sql::internal {

void conflictError(Conflict mode, Constraint k, const std::string& message)
{
    const Error e(ErrorCode::ConstraintViolation, k, message);
    if (mode == Conflict::Fail)
        throw FailError(e);
    if (mode == Conflict::Rollback)
        throw RollbackError(e);
    throw e;
}

namespace {

Conflict effective(Conflict statement, Conflict constraint)
{
    if (statement != Conflict::Default)
        return statement;
    if (constraint != Conflict::Default)
        return constraint;
    return Conflict::Abort;
}

const char* typeName(Type t)
{
    switch (t) {
        case Type::Null: return "NULL";
        case Type::Integer: return "INTEGER";
        case Type::Real: return "REAL";
        case Type::Datetime: return "DATETIME";
        case Type::Text: return "TEXT";
        case Type::Blob: return "BLOB";
    }
    return "NULL";
}

const char* strictName(StrictType t)
{
    switch (t) {
        case StrictType::Any: return "ANY";
        case StrictType::Integer: return "INTEGER";
        case StrictType::Real: return "REAL";
        case StrictType::Text: return "TEXT";
        case StrictType::Blob: return "BLOB";
        case StrictType::Datetime: return "DATETIME";
    }
    return "ANY";
}

std::string uniqueMessage(const Table& t, const std::vector<int>& cols)
{
    std::string m = "UNIQUE constraint failed: ";
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (i)
            m += ", ";
        m += t.name + "." + (cols[i] >= 0 && std::size_t(cols[i]) < t.columns.size()
                                 ? t.columns[std::size_t(cols[i])].name
                                 : std::string("rowid"));
    }
    return m;
}

std::string indexMessage(const Table& t, const Index& ix)
{
    std::vector<int> cols;
    bool expr = false;
    for (const IndexPart& p : ix.parts) {
        cols.push_back(p.column == -2 ? -1 : p.column);
        expr = expr || p.column == -1;
    }
    if (expr)
        return "UNIQUE constraint failed: index '" + ix.name + "'";
    return uniqueMessage(t, cols);
}

std::uint64_t zig(std::int64_t v)
{
    return (std::uint64_t(v) << 1) ^ std::uint64_t(v >> 63);
}

}  // namespace

Writer::Writer(Planner& planner, Program& prog) : planner_(planner), prog_(prog) {}

ExprPtr Writer::bindRowExpr(const std::string& sql, TablePlan& tp, std::size_t base, unsigned flags)
{
    ExprPtr e = parseExpression(sql);
    std::vector<SourceNode> sources;
    SourceNode s;
    s.kind = SourceNode::Kind::Pseudo;
    s.table = tp.table;
    s.name = tp.table->name;
    s.alias = tp.table->name;
    for (const Column& c : tp.table->columns) {
        s.columns.push_back(c.name);
        s.affinity.push_back(c.affinity);
        s.collation.push_back(c.collation);
        s.hidden.push_back(false);
    }
    s.base = base;
    s.width = tp.table->width();
    sources.push_back(std::move(s));
    Scope scope;
    scope.sources = &sources;
    scope.text = sql;
    planner_.resolve(e, scope, flags);
    return e;
}

TablePlan& Writer::plan(const Table& t, std::size_t store)
{
    auto it = plans_.find(&t);
    if (it != plans_.end())
        return *it->second;
    auto tp = std::make_unique<TablePlan>();
    TablePlan& p = *tp;
    plans_.emplace(&t, std::move(tp));
    p.table = &t;
    p.store = store;
    const std::size_t w = t.width();
    p.newBase = planner_.allocate(w);
    p.oldBase = planner_.allocate(w);
    p.excludedBase = planner_.allocate(w);
    const std::size_t n = t.columns.size();
    p.defaults.resize(n);
    p.generated.resize(n);
    p.oldGenerated.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Column& c = t.columns[i];
        if (c.hasDefault && !c.defaultConstant)
            p.defaults[i] = bindRowExpr(c.defaultSql, p, p.newBase, kNoSubquery);
        if (c.generated) {
            p.generated[i] = bindRowExpr(c.generatedSql, p, p.newBase, kNoSubquery | kOnlyDeterministic);
            p.oldGenerated[i] = bindRowExpr(c.generatedSql, p, p.oldBase, kNoSubquery | kOnlyDeterministic);
        }
    }
    for (const Check& ck : t.checks) {
        p.checks.push_back(bindRowExpr(ck.sql, p, p.newBase, kNoSubquery | kOnlyDeterministic));
        p.checkNames.push_back(ck.name.empty() ? ck.sql : ck.name);
    }
    for (const Index* ix : t.indexes) {
        IndexWriter iw;
        iw.index = ix;
        for (const IndexPart& part : ix->parts) {
            if (part.column == -1) {
                iw.newParts.push_back(bindRowExpr(part.exprSql, p, p.newBase, kNoSubquery | kOnlyDeterministic));
                iw.oldParts.push_back(bindRowExpr(part.exprSql, p, p.oldBase, kNoSubquery | kOnlyDeterministic));
            } else {
                iw.newParts.emplace_back();
                iw.oldParts.emplace_back();
            }
        }
        if (!ix->whereSql.empty()) {
            iw.newWhere = bindRowExpr(ix->whereSql, p, p.newBase, kNoSubquery | kOnlyDeterministic);
            iw.oldWhere = bindRowExpr(ix->whereSql, p, p.oldBase, kNoSubquery | kOnlyDeterministic);
        }
        p.indexes.push_back(std::move(iw));
    }
    for (std::size_t s = 0; s < planner_.session().stores.size(); ++s) {
        if (!planner_.session().stores[s].catalog)
            continue;
        for (const Trigger* tr : planner_.session().catalog(s).schema().triggersOn(t.name)) {
            TriggerPlan tpl;
            tpl.trigger = tr;
            for (const std::string& col : tr->updateOf) {
                const int pos = t.find(col);
                if (pos >= 0)
                    tpl.updateOf.push_back(pos);
            }
            p.triggers.push_back(std::move(tpl));
        }
    }
    if (planner_.session().conn->foreignKeys)
        planFks(p);
    return p;
}

void Writer::planFks(TablePlan& tp)
{
    if (tp.fkPlanned)
        return;
    tp.fkPlanned = true;
    const Table& t = *tp.table;
    Session& session = planner_.session();
    auto mismatch = [&](const std::string& parent) -> void {
        throw Error(ErrorCode::Runtime, "foreign key mismatch - \"" + t.name + "\" referencing \"" + parent + "\"");
    };
    // Parent key column positions for an FK, and whether they are the primary
    // key or a unique index.
    auto resolveParent = [&](const ForeignKey& fk, const Table& parent, FkParent& out) {
        out.parent = &parent;
        std::vector<int> cols;
        if (fk.parentColumns.empty()) {
            if (parent.primaryKey.empty() && parent.rowidAlias < 0)
                mismatch(parent.name);
            cols = parent.primaryKey;
        } else {
            for (const std::string& n : fk.parentColumns) {
                const int pos = parent.find(n);
                if (pos < 0 || pos >= int(parent.columns.size()))
                    mismatch(parent.name);
                cols.push_back(pos);
            }
        }
        if (cols.size() != fk.columns.size())
            mismatch(parent.name);
        out.parentColumns = cols;
        std::vector<int> sortedCols = cols, sortedPk = parent.primaryKey;
        std::sort(sortedCols.begin(), sortedCols.end());
        std::sort(sortedPk.begin(), sortedPk.end());
        if (sortedCols == sortedPk) {
            out.parentIndex = nullptr;
            out.parentIsRowid = parent.keyedByRowid();
            return;
        }
        for (const Index* ix : parent.indexes) {
            if (!ix->unique || ix->parts.size() != cols.size() || !ix->whereSql.empty())
                continue;
            std::vector<int> ixCols;
            for (const IndexPart& p : ix->parts)
                ixCols.push_back(p.column);
            std::vector<int> a = ixCols, b = cols;
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (a == b) {
                // Reorder the FK's parent columns into index order.
                out.parentIndex = ix;
                return;
            }
        }
        mismatch(parent.name);
    };
    for (const ForeignKey& fk : t.foreignKeys) {
        std::size_t store = 0;
        const Table* parent = planner_.findTable("", fk.parentTable, store);
        FkParent fp;
        fp.fk = &fk;
        fp.store = store;
        if (!parent) {
            fp.parent = nullptr;
            tp.parents.push_back(fp);
            continue;
        }
        resolveParent(fk, *parent, fp);
        tp.parents.push_back(fp);
    }
    for (std::size_t s = 0; s < session.stores.size(); ++s) {
        if (!session.stores[s].catalog)
            continue;
        for (const Table* child : session.catalog(s).schema().childrenOf(t.name)) {
            for (const ForeignKey& fk : child->foreignKeys) {
                if (!equalsNoCase(fk.parentTable, t.name))
                    continue;
                FkChild fc;
                fc.child = child;
                fc.fk = &fk;
                fc.store = s;
                FkParent probe;
                resolveParent(fk, t, probe);
                fc.parentColumns = probe.parentColumns;
                for (const Index* ix : child->indexes) {
                    if (ix->parts.size() >= fk.columns.size() && ix->whereSql.empty()) {
                        bool lead = true;
                        for (std::size_t k = 0; k < fk.columns.size(); ++k)
                            lead = lead && ix->parts[k].column == fk.columns[k];
                        if (lead) {
                            fc.childIndex = ix;
                            break;
                        }
                    }
                }
                tp.children.push_back(fc);
            }
        }
    }
    // Writers for the child tables, which CASCADE and SET NULL modify.
    for (FkChild& fc : tp.children) {
        if (fc.fk->onDelete != FkAction::NoAction || fc.fk->onUpdate != FkAction::NoAction ||
            true) {
            TablePlan& cp = plan(*fc.child, fc.store);
            fc.plan = &cp;
        }
    }
}

// ----------------------------------------------------------- row prep ----

void Writer::initRow(TablePlan& tp, Ctx& c)
{
    const Table& t = *tp.table;
    Value* row = c.slots + tp.newBase;
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        const Column& col = t.columns[i];
        if (col.hasDefault && col.defaultConstant)
            row[i] = col.defaultValue;
        else if (tp.defaults[i])
            row[i] = eval(*tp.defaults[i], c);
        else
            row[i].setNull();
    }
    if (t.hasRowid())
        row[t.columns.size()].setNull();
}

void Writer::computeGenerated(TablePlan& tp, Ctx& c, std::size_t base, bool stored, bool virt)
{
    const Table& t = *tp.table;
    const std::vector<ExprPtr>& exprs = base == tp.newBase ? tp.generated : tp.oldGenerated;
    // Generated columns may depend on each other; SQLite evaluates them in
    // dependency order. Two passes settle any chain of reasonable length.
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < t.columns.size(); ++i) {
            const Column& col = t.columns[i];
            if (!col.generated || !exprs[i])
                continue;
            if ((col.generated == 2 && !stored) || (col.generated == 1 && !virt))
                continue;
            Value v = eval(*exprs[i], c);
            if (t.strict) {
                if (!strictCoerce(v, col.strictType))
                    conflictError(Conflict::Abort, Constraint::Datatype,
                                  std::string("cannot store ") + typeName(v.type()) + " value in " +
                                      strictName(col.strictType) + " column " + t.name + "." + col.name);
            } else {
                applyAffinity(v, col.affinity);
            }
            c.slots[base + i] = std::move(v);
        }
    }
}

bool Writer::applyColumnRules(TablePlan& tp, Ctx& c, Conflict mode)
{
    const Table& t = *tp.table;
    Value* row = c.slots + tp.newBase;
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        const Column& col = t.columns[i];
        if (col.generated)
            continue;
        Value& v = row[i];
        if (v.type() == Type::Real && std::isnan(v.real()))
            v.setNull();
        if (t.strict) {
            if (!strictCoerce(v, col.strictType)) {
                if (int(i) == t.rowidAlias && v.type() != Type::Integer)
                    throw Error(ErrorCode::ConstraintViolation, Constraint::Datatype, "datatype mismatch");
                conflictError(effective(mode, Conflict::Default) == Conflict::Ignore ? Conflict::Abort : mode,
                              Constraint::Datatype,
                              std::string("cannot store ") + typeName(v.type()) + " value in " +
                                  strictName(col.strictType) + " column " + t.name + "." + col.name);
            }
        } else {
            applyAffinity(v, col.affinity);
        }
    }
    return true;
}

bool Writer::checkConstraints(TablePlan& tp, Ctx& c, Conflict mode)
{
    const Table& t = *tp.table;
    Value* row = c.slots + tp.newBase;
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        const Column& col = t.columns[i];
        if (!col.notNull || !row[i].isNull() || int(i) == t.rowidAlias)
            continue;
        const Conflict m = effective(mode, col.notNullConflict != Conflict::Default
                                               ? col.notNullConflict
                                               : (col.primaryKey ? t.pkConflict : Conflict::Default));
        if (m == Conflict::Ignore)
            return false;
        if (m == Conflict::Replace && col.hasDefault) {
            if (col.defaultConstant)
                row[i] = col.defaultValue;
            else
                row[i] = eval(*tp.defaults[i], c);
            if (!row[i].isNull())
                continue;
        }
        conflictError(m == Conflict::Replace ? Conflict::Abort : m, Constraint::NotNull,
                      "NOT NULL constraint failed: " + t.name + "." + col.name);
    }
    if (!(c.session && c.session->conn && c.session->conn->ignoreCheckConstraints)) {
        for (std::size_t k = 0; k < tp.checks.size(); ++k) {
            const Value& v = eval(*tp.checks[k], c);
            if (v.isNull() || isTrue(v))
                continue;
            const Conflict m = effective(mode, Conflict::Default);
            if (m == Conflict::Ignore)
                return false;
            conflictError(m == Conflict::Replace ? Conflict::Abort : m, Constraint::Check,
                          "CHECK constraint failed: " + tp.checkNames[k]);
        }
    }
    return true;
}

// ------------------------------------------------------------- storage ---

void Writer::storageKey(TablePlan& tp, Ctx& c, std::size_t base, std::string& out)
{
    const Table& t = *tp.table;
    out.clear();
    if (t.keyedByRowid()) {
        appendRowidKey(out, c.slots[base + t.columns.size()].integer());
        return;
    }
    for (std::size_t i = 0; i < t.primaryKey.size(); ++i)
        appendKey(out, c.slots[base + std::size_t(t.primaryKey[i])], t.pkCollation[i], t.pkDesc[i]);
}

bool Writer::loadStored(TablePlan& tp, Ctx& c, std::string_view key, std::size_t base)
{
    const Table& t = *tp.table;
    Catalog& cat = c.session->catalog(tp.store);
    const auto payload = cat.rows(t).get(sliceOf(key));
    if (!payload)
        return false;
    std::string_view p = viewOf(*payload);
    std::int64_t rowid = 0;
    if (t.payloadRowid()) {
        const std::uint64_t z = getVarint(p);
        rowid = std::int64_t((z >> 1) ^ -(z & 1));
    } else if (t.keyedByRowid()) {
        std::string_view k = key;
        rowid = readRowidKey(k);
    }
    Value* row = c.slots + base;
    const std::size_t have = decodeRowInto(p, row, t.columns.size());
    for (std::size_t i = have; i < t.columns.size(); ++i) {
        const Column& col = t.columns[i];
        if (col.hasDefault && col.defaultConstant)
            row[i] = col.defaultValue;
    }
    if (t.hasRowid()) {
        row[t.columns.size()].setInteger(rowid);
        if (t.rowidAlias >= 0)
            row[t.rowidAlias].setInteger(rowid);
    }
    computeGenerated(tp, c, base, false, true);
    return true;
}

bool Writer::writeRow(TablePlan& tp, Ctx& c, const std::string& key, bool unique)
{
    const Table& t = *tp.table;
    const Value* row = c.slots + tp.newBase;
    std::string& out = tp.payload;
    out.clear();
    if (t.payloadRowid())
        putVarint(out, zig(row[t.columns.size()].integer()));
    if (tp.storedNull.size() != t.columns.size()) {
        tp.storedNull.assign(t.columns.size(), 0);
        for (std::size_t i = 0; i < t.columns.size(); ++i)
            tp.storedNull[i] = int(i) == t.rowidAlias || t.columns[i].generated == 1;
    }
    encodeRowInto(out, row, t.columns.size(), tp.storedNull.data());
    return c.session->catalog(tp.store).rows(t).put(sliceOf(key), sliceOf(out),
                                                    unique ? nosql::PutMode::InsertUnique
                                                           : nosql::PutMode::Upsert);
}

namespace {

/// A REAL that a number key reads back as an INTEGER (see decodeKeyInto).
bool wholeReal(const Value& v) noexcept
{
    std::int64_t i = 0;
    return v.type() == Type::Real && (realIsExactInt(v.real(), i) || v.real() == -9223372036854775808.0);
}

void setFlag(std::string& flags, std::size_t bit)
{
    if (flags.size() <= bit / 8)
        flags.resize(bit / 8 + 1, '\0');
    flags[bit / 8] = char(std::uint8_t(flags[bit / 8]) | (1u << (bit % 8)));
}

}  // namespace

bool Writer::entryFor(TablePlan& tp, IndexWriter& iw, Ctx& c, std::size_t base, bool isNew,
                      std::string& out, bool& hasNull, std::string* flags)
{
    const Table& t = *tp.table;
    const Index& ix = *iw.index;
    const ExprPtr& where = isNew ? iw.newWhere : iw.oldWhere;
    if (where && !holds(where.get(), c))
        return false;
    out.clear();
    if (flags)
        flags->clear();
    hasNull = false;
    for (std::size_t k = 0; k < ix.parts.size(); ++k) {
        const IndexPart& p = ix.parts[k];
        const Value* v;
        if (p.column == -1)
            v = &eval(*(isNew ? iw.newParts[k] : iw.oldParts[k]), c);
        else if (p.column == -2)
            v = &c.slots[base + t.columns.size()];
        else
            v = &c.slots[base + std::size_t(p.column)];
        hasNull = hasNull || v->isNull();
        if (flags && wholeReal(*v))
            setFlag(*flags, k);
        appendKey(out, *v, p.collation, p.desc);
    }
    if (flags && !t.keyedByRowid()) {
        for (std::size_t j = 0; j < t.primaryKey.size(); ++j) {
            if (wholeReal(c.slots[base + std::size_t(t.primaryKey[j])]))
                setFlag(*flags, ix.parts.size() + j);
        }
    }
    return true;
}

bool Writer::findConflict(TablePlan& tp, IndexWriter& iw, Ctx& c, const std::string& self, std::string& found)
{
    bool hasNull = false;
    if (!entryFor(tp, iw, c, tp.newBase, true, tp.probe, hasNull) || hasNull)
        return false;
    const nosql::Db& db = c.session->catalog(tp.store).entries(*iw.index);
    nosql::Cursor cur = db.cursor();
    const nosql::Slice prefix = sliceOf(tp.probe);
    for (bool ok = cur.seek(prefix); ok; ok = cur.next()) {
        const std::string_view k = viewOf(cur.key());
        if (k.substr(0, tp.probe.size()) != tp.probe)
            break;
        const std::string_view rest = k.substr(tp.probe.size());
        if (rest != self) {
            found.assign(rest.data(), rest.size());
            return true;
        }
    }
    return false;
}

void Writer::addEntries(TablePlan& tp, Ctx& c, const std::string& key, bool skipUnchanged)
{
    for (IndexWriter& iw : tp.indexes) {
        if (skipUnchanged && iw.unchanged)
            continue;
        bool hasNull = false;
        if (!entryFor(tp, iw, c, tp.newBase, true, tp.entry, hasNull, &tp.flags))
            continue;
        tp.entry += key;
        c.session->catalog(tp.store).entries(*iw.index).put(sliceOf(tp.entry), sliceOf(tp.flags));
    }
}

void Writer::removeEntries(TablePlan& tp, Ctx& c, const std::string& key, bool skipUnchanged)
{
    for (IndexWriter& iw : tp.indexes) {
        if (skipUnchanged && iw.unchanged)
            continue;
        bool hasNull = false;
        if (!entryFor(tp, iw, c, tp.oldBase, false, tp.entry, hasNull))
            continue;
        tp.entry += key;
        c.session->catalog(tp.store).entries(*iw.index).erase(sliceOf(tp.entry));
    }
}

void Writer::backfill(TablePlan& tp, Ctx& c, const Index& ix)
{
    IndexWriter* iw = nullptr;
    for (IndexWriter& w : tp.indexes) {
        if (w.index == &ix)
            iw = &w;
    }
    if (!iw)
        throw Error(ErrorCode::Internal, "index not planned for backfill");
    Catalog& cat = c.session->catalog(tp.store);
    const nosql::Db& rows = cat.rows(*tp.table);
    const nosql::Db& entries = cat.entries(ix);
    std::vector<std::string> keys;
    {
        nosql::Cursor cur = rows.cursor();
        for (bool ok = cur.first(); ok; ok = cur.next())
            keys.emplace_back(viewOf(cur.key()));
    }
    std::string other;
    for (const std::string& key : keys) {
        if (!loadStored(tp, c, key, tp.newBase))
            continue;
        if (ix.unique && findConflict(tp, *iw, c, key, other))
            throw Error(ErrorCode::ConstraintViolation, Constraint::Unique, indexMessage(*tp.table, ix));
        bool hasNull = false;
        if (!entryFor(tp, *iw, c, tp.newBase, true, tp.entry, hasNull, &tp.flags))
            continue;
        tp.entry += key;
        entries.put(sliceOf(tp.entry), sliceOf(tp.flags));
    }
}

std::uint64_t Writer::verifyEntries(TablePlan& tp, Ctx& c, const Index& ix,
                                    const std::function<void(std::int64_t row)>& missing)
{
    IndexWriter* iw = nullptr;
    for (IndexWriter& w : tp.indexes) {
        if (w.index == &ix)
            iw = &w;
    }
    if (!iw)
        throw Error(ErrorCode::Internal, "index not planned for verification");
    Catalog& cat = c.session->catalog(tp.store);
    const Table& t = *tp.table;
    const nosql::Db& entries = cat.entries(ix);
    nosql::Cursor cur = cat.rows(t).cursor();
    std::string key;
    std::uint64_t expected = 0;
    std::int64_t ordinal = 0;
    for (bool ok = cur.first(); ok; ok = cur.next()) {
        ++ordinal;
        key.assign(viewOf(cur.key()));
        if (!loadStored(tp, c, key, tp.newBase))
            continue;
        bool hasNull = false;
        if (!entryFor(tp, *iw, c, tp.newBase, true, tp.entry, hasNull, &tp.flags))
            continue;
        ++expected;
        tp.entry += key;
        const auto stored = entries.get(sliceOf(tp.entry));
        if (!stored || viewOf(*stored) != std::string_view(tp.flags))
            missing(t.hasRowid() ? c.slots[tp.newBase + t.columns.size()].integer() : ordinal);
    }
    return expected;
}

// ------------------------------------------------------------- triggers --

bool Writer::fire(TablePlan& tp, Ctx& c, TriggerTime time, TriggerEvent event,
                  const std::vector<int>* changed)
{
    if (tp.triggers.empty())
        return true;
    for (TriggerPlan& tr : tp.triggers) {
        const Trigger& t = *tr.trigger;
        if (t.time != time || t.event != event)
            continue;
        if (event == TriggerEvent::Update && !tr.updateOf.empty() && changed) {
            bool hit = false;
            for (const int col : tr.updateOf)
                hit = hit || std::find(changed->begin(), changed->end(), col) != changed->end();
            if (!hit)
                continue;
        }
        try {
            runTriggerBody(tr, tp, c);
        } catch (const TriggerIgnore&) {
            return false;
        }
    }
    return true;
}

void Writer::runTriggerBody(TriggerPlan& tr, TablePlan& tp, Ctx& c)
{
    ConnectionState* conn = c.session->conn;
    const int depth = c.depth + 1;
    if (depth > 1000)
        throw Error(ErrorCode::Runtime, "too many levels of trigger recursion");
    // Without recursive triggers, a trigger never fires while it is running.
    std::size_t slot = std::size_t(depth);
    if (!conn->recursiveTriggers) {
        for (const auto& inst : tr.byDepth) {
            if (inst && !inst->body.empty() && inst->body.front()->active)
                return;
        }
        slot = 0;
    }
    if (tr.byDepth.size() <= slot)
        tr.byDepth.resize(slot + 1);
    if (!tr.byDepth[slot] || tr.byDepth[slot]->body.empty() || tr.byDepth[slot]->body.front()->active) {
        // Plan a fresh instance for this depth.
        auto inst = std::make_unique<TriggerInstance>();
        const StatementNode def = parseOne(tr.trigger->sql);
        const auto& ct = std::get<CreateTriggerStmt>(def.body);
        const Table& t = *tp.table;
        auto makeScope = [&](Program& prog, Planner& pl, std::vector<SourceNode>& sources,
                             std::size_t& nb, std::size_t& ob) {
            nb = pl.allocate(t.width());
            ob = pl.allocate(t.width());
            for (int k = 0; k < 2; ++k) {
                SourceNode s;
                s.kind = SourceNode::Kind::Pseudo;
                s.table = &t;
                s.name = k == 0 ? "new" : "old";
                s.alias = s.name;
                for (const Column& col : t.columns) {
                    s.columns.push_back(col.name);
                    s.affinity.push_back(col.affinity);
                    s.collation.push_back(col.collation);
                    s.hidden.push_back(false);
                }
                s.base = k == 0 ? nb : ob;
                s.width = t.width();
                sources.push_back(std::move(s));
            }
            (void)prog;
        };
        for (const auto& stmt : ct.body) {
            auto prog = std::make_unique<Program>();
            prog->text = tr.trigger->sql;
            prog->node = std::make_unique<StatementNode>(parseOne(
                std::string_view(tr.trigger->sql).substr(stmt->begin, stmt->end - stmt->begin)));
            prog->text = std::string(std::string_view(tr.trigger->sql).substr(stmt->begin, stmt->end - stmt->begin));
            Planner pl(*prog, *c.session);
            std::vector<SourceNode> sources;
            std::size_t nb = 0, ob = 0;
            makeScope(*prog, pl, sources, nb, ob);
            Scope scope;
            scope.sources = &sources;
            scope.text = prog->text;
            scope.trigger = true;
            planStatement(*prog, *c.session, &scope);
            inst->newBase.push_back(nb);
            inst->oldBase.push_back(ob);
            inst->body.push_back(std::move(prog));
        }
        if (ct.when) {
            auto prog = std::make_unique<Program>();
            prog->text = tr.trigger->sql;
            Planner pl(*prog, *c.session);
            std::vector<SourceNode> sources;
            makeScope(*prog, pl, sources, inst->whenNew, inst->whenOld);
            Scope scope;
            scope.sources = &sources;
            scope.text = prog->text;
            prog->owned.push_back(parseExpression(std::string_view(tr.trigger->sql)
                                                      .substr(ct.when->begin, ct.when->end - ct.when->begin)));
            scope.text = std::string_view(tr.trigger->sql).substr(ct.when->begin, ct.when->end - ct.when->begin);
            prog->text = std::string(scope.text);
            scope.text = prog->text;
            pl.resolve(prog->owned.back(), scope, 0);
            inst->when = prog->owned.back().get();
            prog->frame.resize(prog->frameSize);
            inst->whenProgram = std::move(prog);
        }
        tr.byDepth[slot] = std::move(inst);
    }
    TriggerInstance& inst = *tr.byDepth[slot];
    const std::size_t w = tp.table->width();
    if (inst.whenProgram) {
        Program& wp = *inst.whenProgram;
        wp.frame.resize(wp.frameSize);
        for (std::size_t i = 0; i < w; ++i) {
            wp.frame[inst.whenNew + i] = c.slots[tp.newBase + i];
            wp.frame[inst.whenOld + i] = c.slots[tp.oldBase + i];
        }
        Ctx wc = c;
        wc.slots = wp.frame.data();
        wc.params = nullptr;
        wc.run = ++c.session->conn->runCounter;
        if (!holds(inst.when, wc))
            return;
    }
    const std::int64_t savedRowid = conn->lastInsertRowid;
    for (std::size_t k = 0; k < inst.body.size(); ++k) {
        Program& p = *inst.body[k];
        p.frame.resize(p.frameSize);
        for (std::size_t i = 0; i < w; ++i) {
            p.frame[inst.newBase[k] + i] = c.slots[tp.newBase + i];
            p.frame[inst.oldBase[k] + i] = c.slots[tp.oldBase + i];
        }
        p.active = true;
        try {
            const std::int64_t n = runNested(p, *c.session, depth, *c.fctx);
            nested += n;
        } catch (...) {
            p.active = false;
            conn->lastInsertRowid = savedRowid;
            throw;
        }
        p.active = false;
    }
    conn->lastInsertRowid = savedRowid;
}

// ----------------------------------------------------------- foreign keys

namespace {

bool fkEnabled(Ctx& c)
{
    return c.session->conn->foreignKeys;
}

std::int64_t& fkCounter(Ctx& c, const ForeignKey& fk)
{
    if (fk.deferred || c.session->conn->deferForeignKeys)
        return c.session->deferredFkViolations;
    return c.session->immediateFkViolations;
}

}  // namespace

/// For the child row in slots[base...]: does its parent exist? Adjusts the
/// violation counter by `delta` (+1 when the row is being added, -1 when
/// removed) for each FK whose parent is missing.
void Writer::fkChildCheck(TablePlan& tp, Ctx& c, std::size_t base, int delta, const std::vector<int>* changed)
{
    if (!fkEnabled(c))
        return;
    const Table& t = *tp.table;
    for (FkParent& fp : tp.parents) {
        const ForeignKey& fk = *fp.fk;
        if (changed) {
            bool hit = false;
            for (const int col : fk.columns)
                hit = hit || std::find(changed->begin(), changed->end(), col) != changed->end();
            if (!hit)
                continue;
        }
        bool anyNull = false;
        for (const int col : fk.columns)
            anyNull = anyNull || c.slots[base + std::size_t(col)].isNull();
        if (anyNull)
            continue;
        if (!fp.parent)
            throw Error(ErrorCode::NoSuchTable, "no such table: main." + fk.parentTable);
        const Table& parent = *fp.parent;
        // Look the parent key up, with the parent columns' affinity applied.
        std::string key;
        bool found = false;
        Catalog& pcat = c.session->catalog(fp.store);
        auto childValue = [&](std::size_t k) {
            Value v = c.slots[base + std::size_t(fk.columns[k])];
            const int pcol = fp.parentColumns[k];
            if (pcol >= 0 && std::size_t(pcol) < parent.columns.size())
                applyAffinity(v, parent.columns[std::size_t(pcol)].affinity);
            return v;
        };
        if (!fp.parentIndex) {
            if (fp.parentIsRowid) {
                const Value v = childValue(0);
                if (v.type() == Type::Integer) {
                    appendRowidKey(key, v.integer());
                    found = pcat.rows(parent).get(sliceOf(key)).has_value();
                }
            } else {
                for (std::size_t i = 0; i < parent.primaryKey.size(); ++i) {
                    std::size_t k = 0;
                    while (k < fp.parentColumns.size() && fp.parentColumns[k] != parent.primaryKey[i])
                        ++k;
                    appendKey(key, childValue(k), parent.pkCollation[i], parent.pkDesc[i]);
                }
                found = pcat.rows(parent).get(sliceOf(key)).has_value();
            }
        } else {
            for (const IndexPart& p : fp.parentIndex->parts) {
                std::size_t k = 0;
                while (k < fp.parentColumns.size() && fp.parentColumns[k] != p.column)
                    ++k;
                appendKey(key, childValue(k), p.collation, p.desc);
            }
            nosql::Cursor cur = pcat.entries(*fp.parentIndex).cursor();
            found = cur.seek(sliceOf(key)) && viewOf(cur.key()).substr(0, key.size()) == key;
        }
        if (found)
            continue;
        if (delta > 0 && !(fk.deferred || c.session->conn->deferForeignKeys) && !c.session->conn->deferForeignKeys) {
            // Immediate constraints are counted and checked when the
            // statement ends, as SQLite does.
        }
        fkCounter(c, fk) += delta;
        (void)t;
    }
}

std::int64_t Writer::countChildren(FkChild& fc, Ctx& c, std::size_t parentBase, TablePlan& parentPlan)
{
    (void)parentPlan;
    const Table& child = *fc.child;
    const ForeignKey& fk = *fc.fk;
    std::vector<Value> want;
    for (std::size_t k = 0; k < fk.columns.size(); ++k) {
        const Value& v = c.slots[parentBase + std::size_t(fc.parentColumns[k])];
        if (v.isNull())
            return 0;
        want.push_back(v);
    }
    std::int64_t n = 0;
    TablePlan& cp = *fc.plan;
    Catalog& cat = c.session->catalog(fc.store);
    nosql::Cursor cur = cat.rows(child).cursor();
    std::vector<std::string> keys;
    for (bool ok = cur.first(); ok; ok = cur.next()) {
        const std::string key(viewOf(cur.key()));
        if (!loadStored(cp, c, key, cp.oldBase))
            continue;
        bool match = true;
        for (std::size_t k = 0; k < fk.columns.size() && match; ++k) {
            Value v = c.slots[cp.oldBase + std::size_t(fk.columns[k])];
            if (v.isNull()) {
                match = false;
                break;
            }
            Value p = want[k];
            match = compareValues(v, p) == 0;
            if (!match) {
                Value conv = v;
                applyComparisonAffinity(conv, Affinity::Numeric);
                match = compareValues(conv, p) == 0;
            }
        }
        if (match)
            ++n;
    }
    return n;
}

void Writer::fkParentActions(TablePlan& tp, Ctx& c, bool isDelete, const std::vector<int>* changed)
{
    if (!fkEnabled(c))
        return;
    for (FkChild& fc : tp.children) {
        const ForeignKey& fk = *fc.fk;
        if (!isDelete && changed) {
            bool hit = false;
            for (const int col : fc.parentColumns)
                hit = hit || std::find(changed->begin(), changed->end(), col) != changed->end();
            if (!hit)
                continue;
            bool same = true;
            for (const int col : fc.parentColumns)
                same = same && compareValues(c.slots[tp.oldBase + std::size_t(col)],
                                             c.slots[tp.newBase + std::size_t(col)]) == 0;
            if (same)
                continue;
        }
        const FkAction action = isDelete ? fk.onDelete : fk.onUpdate;
        TablePlan& cp = *fc.plan;
        const Table& child = *fc.child;
        // Matching child rows, by key, collected before anything changes.
        std::vector<std::string> keys;
        {
            std::vector<Value> want;
            bool anyNull = false;
            for (const int col : fc.parentColumns) {
                want.push_back(c.slots[tp.oldBase + std::size_t(col)]);
                anyNull = anyNull || want.back().isNull();
            }
            if (anyNull)
                continue;
            Catalog& cat = c.session->catalog(fc.store);
            nosql::Cursor cur = cat.rows(child).cursor();
            for (bool ok = cur.first(); ok; ok = cur.next()) {
                const std::string key(viewOf(cur.key()));
                if (!loadStored(cp, c, key, cp.oldBase))
                    continue;
                bool match = true;
                for (std::size_t k = 0; k < fk.columns.size() && match; ++k) {
                    Value v = c.slots[cp.oldBase + std::size_t(fk.columns[k])];
                    const int pcol = fc.parentColumns[k];
                    if (std::size_t(pcol) < tp.table->columns.size())
                        applyComparisonAffinity(v, tp.table->columns[std::size_t(pcol)].affinity);
                    match = !v.isNull() && compareValues(v, want[k]) == 0;
                }
                if (match)
                    keys.push_back(key);
            }
        }
        if (keys.empty())
            continue;
        switch (action) {
            case FkAction::NoAction:
                fkCounter(c, fk) += std::int64_t(keys.size());
                break;
            case FkAction::Restrict:
                throw Error(ErrorCode::ConstraintViolation, Constraint::ForeignKey,
                            "FOREIGN KEY constraint failed");
            case FkAction::Cascade:
                for (const std::string& key : keys) {
                    if (!loadStored(cp, c, key, cp.oldBase))
                        continue;
                    if (isDelete) {
                        remove(cp, c, key);
                    } else {
                        for (std::size_t i = 0; i < child.width(); ++i)
                            c.slots[cp.newBase + i] = c.slots[cp.oldBase + i];
                        std::vector<int> cols;
                        for (std::size_t k = 0; k < fk.columns.size(); ++k) {
                            c.slots[cp.newBase + std::size_t(fk.columns[k])] =
                                c.slots[tp.newBase + std::size_t(fc.parentColumns[k])];
                            cols.push_back(fk.columns[k]);
                        }
                        update(cp, c, Conflict::Abort, key, &cols);
                    }
                }
                break;
            case FkAction::SetNull:
            case FkAction::SetDefault:
                for (const std::string& key : keys) {
                    if (!loadStored(cp, c, key, cp.oldBase))
                        continue;
                    for (std::size_t i = 0; i < child.width(); ++i)
                        c.slots[cp.newBase + i] = c.slots[cp.oldBase + i];
                    std::vector<int> cols;
                    for (const int col : fk.columns) {
                        Value& v = c.slots[cp.newBase + std::size_t(col)];
                        const Column& cc = child.columns[std::size_t(col)];
                        if (action == FkAction::SetDefault && cc.hasDefault)
                            v = cc.defaultConstant ? cc.defaultValue : eval(*cp.defaults[std::size_t(col)], c);
                        else
                            v.setNull();
                        cols.push_back(col);
                    }
                    update(cp, c, Conflict::Abort, key, &cols);
                }
                break;
        }
    }
}

// ------------------------------------------------------------- INSERT ----

bool Writer::insert(TablePlan& tp, Ctx& c, Conflict mode, std::vector<UpsertPlan>* upserts,
                    std::int64_t& rowid, bool fireTriggers)
{
    const Table& t = *tp.table;
    Catalog& cat = c.session->catalog(tp.store);
    Value* row = c.slots + tp.newBase;
    if (tp.view) {
        // INSTEAD OF INSERT.
        for (std::size_t i = 0; i < t.width(); ++i)
            c.slots[tp.oldBase + i].setNull();
        return fire(tp, c, TriggerTime::InsteadOf, TriggerEvent::Insert, nullptr);
    }
    applyColumnRules(tp, c, mode);
    const std::size_t rs = t.columns.size();
    if (t.hasRowid()) {
        Value& r = row[rs];
        if (t.rowidAlias >= 0)
            r = row[t.rowidAlias];
        if (!r.isNull()) {
            applyAffinity(r, Affinity::Integer);
            if (r.type() != Type::Integer)
                throw Error(ErrorCode::ConstraintViolation, Constraint::Datatype, "datatype mismatch");
            if (t.rowidAlias >= 0)
                row[t.rowidAlias] = r;
        }
    }
    if (fireTriggers && !tp.triggers.empty()) {
        // BEFORE INSERT sees NEW.rowid as -1 until one is assigned.
        const bool assigned = !t.hasRowid() || !row[rs].isNull();
        if (!assigned) {
            row[rs].setInteger(-1);
            if (t.rowidAlias >= 0)
                row[t.rowidAlias].setInteger(-1);
        }
        computeGenerated(tp, c, tp.newBase, true, true);
        for (std::size_t i = 0; i < t.width(); ++i)
            c.slots[tp.oldBase + i].setNull();
        if (!fire(tp, c, TriggerTime::Before, TriggerEvent::Insert, nullptr))
            return false;
        if (!assigned) {
            row[rs].setNull();
            if (t.rowidAlias >= 0)
                row[t.rowidAlias].setNull();
        }
        applyColumnRules(tp, c, mode);
    }
    // A rowid this insert assigns lies past every key in use, so the key
    // cannot collide.
    bool assignedKey = false;
    if (t.hasRowid() && row[rs].isNull()) {
        row[rs].setInteger(cat.nextRowid(t));
        if (t.rowidAlias >= 0)
            row[t.rowidAlias] = row[rs];
        assignedKey = t.keyedByRowid();
    }
    computeGenerated(tp, c, tp.newBase, true, true);
    if (!checkConstraints(tp, c, mode))
        return false;

    std::string& key = tp.key;
    storageKey(tp, c, tp.newBase, key);

    // PRIMARY KEY.
    auto upsertFor = [&](const Index* ix) -> UpsertPlan* {
        if (!upserts)
            return nullptr;
        for (UpsertPlan& u : *upserts) {
            if (u.any || (ix ? u.index == ix : u.primaryKey))
                return &u;
        }
        return nullptr;
    };
    auto doUpsert = [&](UpsertPlan& u, const std::string& existing) -> bool {
        // excluded = the new row; the table's columns = the existing row.
        if (u.doNothing)
            return false;
        for (std::size_t i = 0; i < t.width(); ++i)
            c.slots[tp.excludedBase + i] = row[i];
        if (!loadStored(tp, c, existing, tp.oldBase))
            return false;
        if (u.where && !holds(u.where, c))
            return false;
        std::vector<Value> values;
        for (auto& [col, e] : u.set)
            values.push_back(eval(*e, c));
        for (std::size_t i = 0; i < t.width(); ++i)
            row[i] = c.slots[tp.oldBase + i];
        std::vector<int> changed;
        for (std::size_t k = 0; k < u.set.size(); ++k) {
            const int col = u.set[k].first;
            row[col == int(rs) ? rs : std::size_t(col)] = std::move(values[k]);
            if (col == t.rowidAlias)
                row[rs] = row[col];
            changed.push_back(col);
        }
        upserted = update(tp, c, mode == Conflict::Default ? Conflict::Abort : mode, existing, &changed);
        return false;
    };
    // Under ABORT/FAIL/ROLLBACK a key collision is only an error, so the
    // insert-unique put below finds it without a lookup of its own.
    const Conflict pkMode = effective(mode, t.pkConflict);
    const bool deferKeyCheck = assignedKey || (!upsertFor(nullptr) && pkMode != Conflict::Ignore &&
                                               pkMode != Conflict::Replace);
    auto keyError = [&] {
        std::vector<int> cols = t.primaryKey;
        if (cols.empty() || t.rowidAlias >= 0)
            cols = {t.rowidAlias >= 0 ? t.rowidAlias : -1};
        conflictError(pkMode, Constraint::PrimaryKey, uniqueMessage(t, cols));
    };
    if (!deferKeyCheck && cat.rows(t).get(sliceOf(key))) {
        if (UpsertPlan* u = upsertFor(nullptr)) {
            if (!u->targetWhere || holds(u->targetWhere, c)) {
                const std::string existing = key;
                doUpsert(*u, existing);
                rowid = 0;
                return false;
            }
        }
        const Conflict m = effective(mode, t.pkConflict);
        if (m == Conflict::Ignore)
            return false;
        if (m == Conflict::Replace) {
            const std::string existing = key;
            if (loadStored(tp, c, existing, tp.oldBase))
                remove(tp, c, existing, c.session->conn->recursiveTriggers);
            storageKey(tp, c, tp.newBase, key);
        } else {
            std::vector<int> cols = t.primaryKey;
            if (cols.empty() || t.rowidAlias >= 0)
                cols = {t.rowidAlias >= 0 ? t.rowidAlias : -1};
            conflictError(m, Constraint::PrimaryKey, uniqueMessage(t, cols));
        }
    }
    // UNIQUE indexes.
    std::string other;
    for (IndexWriter& iw : tp.indexes) {
        if (!iw.index->unique)
            continue;
        while (findConflict(tp, iw, c, key, other)) {
            if (UpsertPlan* u = upsertFor(iw.index)) {
                const std::string existing = other;
                doUpsert(*u, existing);
                return false;
            }
            const Conflict m = effective(mode, iw.index->onConflict);
            if (m == Conflict::Ignore)
                return false;
            if (m == Conflict::Replace) {
                const std::string existing = other;
                if (loadStored(tp, c, existing, tp.oldBase))
                    remove(tp, c, existing, c.session->conn->recursiveTriggers);
                continue;
            }
            // SQLite reports a key collision before a UNIQUE one.
            if (deferKeyCheck && !assignedKey && cat.rows(t).get(sliceOf(key)))
                keyError();
            conflictError(m, Constraint::Unique, indexMessage(t, *iw.index));
        }
    }
    if (!writeRow(tp, c, key, deferKeyCheck))
        keyError();
    addEntries(tp, c, key);
    if (t.hasRowid()) {
        rowid = row[rs].integer();
        cat.noteRowid(t, rowid);
    }
    if (fkEnabled(c)) {
        fkChildCheck(tp, c, tp.newBase, +1, nullptr);
        // A new parent row may satisfy children that were waiting for it.
        for (FkChild& fc : tp.children) {
            std::int64_t& counter = fkCounter(c, *fc.fk);
            if (counter > 0)
                counter -= countChildren(fc, c, tp.newBase, tp);
        }
    }
    if (fireTriggers && !tp.triggers.empty()) {
        for (std::size_t i = 0; i < t.width(); ++i)
            c.slots[tp.oldBase + i].setNull();
        fire(tp, c, TriggerTime::After, TriggerEvent::Insert, nullptr);
    }
    return true;
}

// ------------------------------------------------------------- UPDATE ----

bool Writer::update(TablePlan& tp, Ctx& c, Conflict mode, const std::string& oldKey,
                    const std::vector<int>* changed, bool fireTriggers)
{
    const Table& t = *tp.table;
    Catalog& cat = c.session->catalog(tp.store);
    Value* row = c.slots + tp.newBase;
    if (tp.view)
        return fire(tp, c, TriggerTime::InsteadOf, TriggerEvent::Update, changed);
    const std::size_t rs = t.columns.size();
    applyColumnRules(tp, c, mode);
    if (t.hasRowid()) {
        Value& r = row[rs];
        if (t.rowidAlias >= 0)
            r = row[t.rowidAlias];
        if (r.isNull()) {
            // Setting the rowid to NULL assigns a new one, as in SQLite.
            r.setInteger(cat.nextRowid(t));
        }
        applyAffinity(r, Affinity::Integer);
        if (r.type() != Type::Integer)
            throw Error(ErrorCode::ConstraintViolation, Constraint::Datatype, "datatype mismatch");
        if (t.rowidAlias >= 0)
            row[t.rowidAlias] = r;
    }
    computeGenerated(tp, c, tp.newBase, true, true);
    if (fireTriggers && !tp.triggers.empty()) {
        if (!fire(tp, c, TriggerTime::Before, TriggerEvent::Update, changed))
            return false;
        // A BEFORE trigger may have changed the row; reload it.
        if (!loadStored(tp, c, oldKey, tp.oldBase))
            return false;
    }
    if (!checkConstraints(tp, c, mode))
        return false;
    std::string& key = tp.key;
    storageKey(tp, c, tp.newBase, key);
    const bool moved = key != oldKey;
    if (moved && cat.rows(t).get(sliceOf(key))) {
        const Conflict m = effective(mode, t.pkConflict);
        if (m == Conflict::Ignore)
            return false;
        if (m == Conflict::Replace) {
            // Delete the row that stands in the way, keeping our old values.
            std::vector<Value> saved(c.slots + tp.oldBase, c.slots + tp.oldBase + t.width());
            const std::string victim = key;
            if (loadStored(tp, c, victim, tp.oldBase))
                remove(tp, c, victim, c.session->conn->recursiveTriggers);
            for (std::size_t i = 0; i < saved.size(); ++i)
                c.slots[tp.oldBase + i] = saved[i];
            storageKey(tp, c, tp.newBase, key);
        } else {
            std::vector<int> cols = t.primaryKey;
            if (cols.empty() || t.rowidAlias >= 0)
                cols = {t.rowidAlias >= 0 ? t.rowidAlias : -1};
            conflictError(m, Constraint::PrimaryKey, uniqueMessage(t, cols));
        }
    }
    // An index whose entry the update does not change needs no conflict probe
    // and, unless the row moves, no rewrite.
    for (IndexWriter& iw : tp.indexes) {
        bool hasNull = false;
        const bool wasIn = entryFor(tp, iw, c, tp.oldBase, false, iw.oldEntry, hasNull, &iw.oldFlags);
        const bool isIn = entryFor(tp, iw, c, tp.newBase, true, tp.entry, hasNull, &tp.flags);
        iw.unchanged = wasIn == isIn && (!wasIn || (iw.oldEntry == tp.entry && iw.oldFlags == tp.flags));
    }
    std::string other;
    for (IndexWriter& iw : tp.indexes) {
        if (!iw.index->unique || iw.unchanged)
            continue;
        while (findConflict(tp, iw, c, oldKey, other)) {
            if (other == key)
                break;
            const Conflict m = effective(mode, iw.index->onConflict);
            if (m == Conflict::Ignore)
                return false;
            if (m == Conflict::Replace) {
                std::vector<Value> saved(c.slots + tp.oldBase, c.slots + tp.oldBase + t.width());
                const std::string victim = other;
                if (loadStored(tp, c, victim, tp.oldBase))
                    remove(tp, c, victim, c.session->conn->recursiveTriggers);
                for (std::size_t i = 0; i < saved.size(); ++i)
                    c.slots[tp.oldBase + i] = saved[i];
                continue;
            }
            conflictError(m, Constraint::Unique, indexMessage(t, *iw.index));
        }
    }
    removeEntries(tp, c, oldKey, !moved);
    if (moved) {
        cat.rows(t).erase(sliceOf(oldKey));
        if (t.hasRowid())
            cat.rowidRemoved(t, c.slots[tp.oldBase + rs].integer());
    }
    writeRow(tp, c, key);
    addEntries(tp, c, key, !moved);
    if (t.hasRowid())
        cat.noteRowid(t, row[rs].integer());
    if (fkEnabled(c)) {
        // Child side: the old values stop counting, the new ones start.
        fkChildCheck(tp, c, tp.oldBase, -1, changed);
        fkChildCheck(tp, c, tp.newBase, +1, changed);
        fkParentActions(tp, c, false, changed);
        for (FkChild& fc : tp.children) {
            std::int64_t& counter = fkCounter(c, *fc.fk);
            if (counter > 0)
                counter -= countChildren(fc, c, tp.newBase, tp);
        }
    }
    if (fireTriggers && !tp.triggers.empty())
        fire(tp, c, TriggerTime::After, TriggerEvent::Update, changed);
    return true;
}

// ------------------------------------------------------------- DELETE ----

bool Writer::remove(TablePlan& tp, Ctx& c, const std::string& key, bool fireTriggers)
{
    const Table& t = *tp.table;
    Catalog& cat = c.session->catalog(tp.store);
    if (tp.view)
        return fire(tp, c, TriggerTime::InsteadOf, TriggerEvent::Delete, nullptr);
    // Delete triggers see NEW as NULL. The slots may hold the row an
    // INSERT/UPDATE OR REPLACE is about to write, so keep it aside.
    const bool triggers = fireTriggers && !tp.triggers.empty();
    // A cascade through a self-referencing key reuses these slots too.
    const bool keep = triggers || fkEnabled(c);
    std::vector<Value> saved;
    if (keep)
        saved.assign(c.slots + tp.newBase, c.slots + tp.newBase + t.width());
    if (triggers) {
        for (std::size_t i = 0; i < t.width(); ++i)
            c.slots[tp.newBase + i].setNull();
        const bool go = fire(tp, c, TriggerTime::Before, TriggerEvent::Delete, nullptr) &&
                        loadStored(tp, c, key, tp.oldBase);
        if (!go) {
            for (std::size_t i = 0; i < t.width(); ++i)
                c.slots[tp.newBase + i] = saved[i];
            return false;
        }
    }
    if (fkEnabled(c))
        fkParentActions(tp, c, true, nullptr);
    removeEntries(tp, c, key);
    cat.rows(t).erase(sliceOf(key));
    if (t.hasRowid())
        cat.rowidRemoved(t, c.slots[tp.oldBase + t.columns.size()].integer());
    if (fkEnabled(c))
        fkChildCheck(tp, c, tp.oldBase, -1, nullptr);
    if (triggers)
        fire(tp, c, TriggerTime::After, TriggerEvent::Delete, nullptr);
    if (keep) {
        for (std::size_t i = 0; i < t.width(); ++i)
            c.slots[tp.newBase + i] = saved[i];
    }
    return true;
}

}  // namespace sql::internal
