// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "internal/schema.hpp"

#include <algorithm>
#include <atomic>

#include "internal/parser.hpp"
#include "internal/util.hpp"

namespace sql::internal {

namespace {

[[noreturn]] void invalid(const std::string& what)
{
    throw Error(ErrorCode::InvalidArgument, what);
}

template <class T>
const T* findByName(const std::vector<std::unique_ptr<T>>& v, std::string_view name) noexcept
{
    for (const auto& p : v) {
        if (equalsNoCase(p->name, name))
            return p.get();
    }
    return nullptr;
}

/// The text of `e` as it appears in `source` (for stored expressions).
std::string spanText(std::string_view source, const Expr& e)
{
    if (e.end <= e.begin || e.end > source.size())
        return {};
    return std::string(source.substr(e.begin, e.end - e.begin));
}

}  // namespace

bool Index::hasExpressions() const noexcept
{
    for (const IndexPart& p : parts) {
        if (p.column == -1)
            return true;
    }
    return false;
}

int Table::find(std::string_view column) const noexcept
{
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (equalsNoCase(columns[i].name, column))
            return int(i);
    }
    if (hasRowid() && (equalsNoCase(column, "rowid") || equalsNoCase(column, "oid") ||
                       equalsNoCase(column, "_rowid_")))
        return rowidAlias >= 0 ? rowidAlias : int(columns.size());
    return -1;
}

bool Table::hasGenerated() const noexcept
{
    for (const Column& c : columns) {
        if (c.generated)
            return true;
    }
    return false;
}

const Table* Schema::findTable(std::string_view name) const noexcept
{
    return findByName(tables, name);
}
const Index* Schema::findIndex(std::string_view name) const noexcept
{
    return findByName(indexes, name);
}
const View* Schema::findView(std::string_view name) const noexcept
{
    return findByName(views, name);
}
const Trigger* Schema::findTrigger(std::string_view name) const noexcept
{
    return findByName(triggers, name);
}

std::vector<const Trigger*> Schema::triggersOn(std::string_view table) const
{
    std::vector<const Trigger*> out;
    for (const auto& t : triggers) {
        if (equalsNoCase(t->table, table))
            out.push_back(t.get());
    }
    return out;
}

std::vector<const Table*> Schema::childrenOf(std::string_view parent) const
{
    std::vector<const Table*> out;
    for (const auto& t : tables) {
        for (const ForeignKey& fk : t->foreignKeys) {
            if (equalsNoCase(fk.parentTable, parent)) {
                out.push_back(t.get());
                break;
            }
        }
    }
    return out;
}

std::shared_ptr<Schema> Schema::clone() const
{
    auto s = std::make_shared<Schema>();
    s->version = version;
    s->userVersion = userVersion;
    s->applicationId = applicationId;
    for (const auto& t : tables)
        s->tables.push_back(std::make_unique<Table>(*t));
    for (const auto& i : indexes)
        s->indexes.push_back(std::make_unique<Index>(*i));
    for (const auto& v : views)
        s->views.push_back(std::make_unique<View>(*v));
    for (const auto& t : triggers)
        s->triggers.push_back(std::make_unique<Trigger>(*t));
    s->settle();
    return s;
}

std::uint64_t nextSchemaStamp() noexcept
{
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

void Schema::settle()
{
    std::sort(tables.begin(), tables.end(), [](const auto& a, const auto& b) {
        return toLower(a->name) < toLower(b->name);
    });
    for (std::size_t i = 0; i < tables.size(); ++i) {
        tables[i]->position = unsigned(i);
        tables[i]->indexes.clear();
    }
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        indexes[i]->position = unsigned(i);
        for (auto& t : tables) {
            if (equalsNoCase(t->name, indexes[i]->table)) {
                t->indexes.push_back(indexes[i].get());
                break;
            }
        }
    }
}

std::string normalizedCreateSql(std::string_view source, const StatementNode& node,
                                std::uint32_t nameBegin, const char* prefix)
{
    std::string out = prefix;
    if (nameBegin >= node.begin && nameBegin <= node.end)
        out.append(source.substr(nameBegin, node.end - nameBegin));
    return out;
}

// --------------------------------------------------------------- tables ---

Table buildTable(const CreateTableStmt& s, std::string sql)
{
    Table t;
    t.name = s.name;
    t.sql = std::move(sql);
    t.strict = s.strict;
    t.withoutRowid = s.withoutRowid;
    if (s.columns.empty())
        invalid("table " + s.name + " has no columns");
    // Expression spans index into the text `s` was parsed from, which is
    // always the stored SQL itself (the catalog parses what it stores).
    const std::string_view src = t.sql;

    bool sawPk = false;
    for (const ColumnDef& d : s.columns) {
        for (const Column& c : t.columns) {
            if (equalsNoCase(c.name, d.name))
                invalid("duplicate column name: " + d.name);
        }
        Column c;
        c.name = d.name;
        c.declType = d.type;
        c.affinity = affinityOfTypeName(d.type);
        if (s.strict) {
            if (d.type.empty())
                invalid("missing datatype for " + s.name + "." + d.name);
            const auto st = strictTypeOfName(d.type);
            if (!st)
                invalid("unknown datatype for " + s.name + "." + d.name + ": \"" + d.type + "\"");
            c.strictType = *st;
            switch (*st) {
                case StrictType::Integer: c.affinity = Affinity::Integer; break;
                case StrictType::Real: c.affinity = Affinity::Real; break;
                case StrictType::Text: c.affinity = Affinity::Text; break;
                case StrictType::Blob:
                case StrictType::Any:
                case StrictType::Datetime: c.affinity = Affinity::Blob; break;
            }
        }
        if (!d.collation.empty()) {
            const auto coll = collationOfName(d.collation);
            if (!coll)
                invalid("no such collation sequence: " + d.collation);
            c.collation = *coll;
        }
        c.notNull = d.notNull;
        c.declaredNotNull = d.notNull;
        c.notNullConflict = d.notNullConflict;
        if (d.defaultExpr) {
            if (d.generated)
                invalid("cannot use DEFAULT on a generated column");
            c.hasDefault = true;
            c.defaultSql = spanText(src, *d.defaultExpr);
            if (d.defaultExpr->kind == ExprKind::Literal) {
                c.defaultConstant = true;
                c.defaultValue = d.defaultExpr->literal;
            } else {
                c.defaultConstant = false;
            }
        }
        if (d.generated) {
            c.generated = d.stored ? 2 : 1;
            c.generatedSql = spanText(src, *d.generated);
        }
        if (d.primaryKey) {
            if (sawPk)
                invalid("table \"" + s.name + "\" has more than one primary key");
            sawPk = true;
            if (d.generated)
                invalid("generated columns cannot be part of the PRIMARY KEY");
            t.primaryKey.push_back(int(t.columns.size()));
            t.pkDesc.push_back(d.pkDesc);
            t.pkCollation.push_back(c.collation);
            t.pkConflict = d.pkConflict;
            t.autoincrement = d.autoincrement;
        }
        for (const CheckDef& ck : d.checks)
            t.checks.push_back(Check{ck.name, spanText(src, *ck.expr)});
        for (const ForeignKeyDef& fk : d.references) {
            ForeignKey f;
            f.columns.push_back(int(t.columns.size()));
            f.parentTable = fk.table;
            f.parentColumns = fk.parentColumns;
            f.onDelete = fk.onDelete;
            f.onUpdate = fk.onUpdate;
            f.deferred = fk.deferred;
            t.foreignKeys.push_back(std::move(f));
        }
        t.columns.push_back(std::move(c));
    }

    for (const TableConstraint& tc : s.constraints) {
        switch (tc.kind) {
            case TableConstraint::Kind::PrimaryKey: {
                if (sawPk)
                    invalid("table \"" + s.name + "\" has more than one primary key");
                sawPk = true;
                for (const IndexedColumn& ic : tc.columns) {
                    if (ic.expr->kind != ExprKind::Column || !ic.expr->table.empty())
                        invalid("expressions prohibited in PRIMARY KEY and UNIQUE constraints");
                    const int pos = t.find(ic.expr->name);
                    if (pos < 0 || pos >= int(t.columns.size()))
                        throw Error(ErrorCode::NoSuchColumn, "no such column: " + ic.expr->name);
                    if (t.columns[std::size_t(pos)].generated)
                        invalid("generated columns cannot be part of the PRIMARY KEY");
                    if (std::find(t.primaryKey.begin(), t.primaryKey.end(), pos) !=
                        t.primaryKey.end())
                        continue;
                    t.primaryKey.push_back(pos);
                    t.pkDesc.push_back(ic.desc);
                    Collation coll = t.columns[std::size_t(pos)].collation;
                    if (!ic.collation.empty()) {
                        const auto c = collationOfName(ic.collation);
                        if (!c)
                            invalid("no such collation sequence: " + ic.collation);
                        coll = *c;
                    }
                    t.pkCollation.push_back(coll);
                }
                t.pkConflict = tc.conflict;
                t.autoincrement = tc.autoincrement;
                break;
            }
            case TableConstraint::Kind::Check:
                t.checks.push_back(Check{tc.name, spanText(src, *tc.check)});
                break;
            case TableConstraint::Kind::ForeignKey: {
                ForeignKey f;
                for (const std::string& cn : tc.fk.columns) {
                    const int pos = t.find(cn);
                    if (pos < 0 || pos >= int(t.columns.size()))
                        invalid("unknown column \"" + cn + "\" in foreign key definition");
                    f.columns.push_back(pos);
                }
                f.parentTable = tc.fk.table;
                f.parentColumns = tc.fk.parentColumns;
                if (!f.parentColumns.empty() && f.parentColumns.size() != f.columns.size())
                    invalid("number of columns in foreign key does not match the number of "
                            "columns in the referenced table");
                f.onDelete = tc.fk.onDelete;
                f.onUpdate = tc.fk.onUpdate;
                f.deferred = tc.fk.deferred;
                t.foreignKeys.push_back(std::move(f));
                break;
            }
            case TableConstraint::Kind::Unique:
                for (const IndexedColumn& ic : tc.columns) {
                    if (ic.expr->kind != ExprKind::Column || !ic.expr->table.empty())
                        invalid("expressions prohibited in PRIMARY KEY and UNIQUE constraints");
                    if (t.find(ic.expr->name) < 0)
                        throw Error(ErrorCode::NoSuchColumn, "no such column: " + ic.expr->name);
                }
                break;
        }
    }

    // INTEGER PRIMARY KEY (exactly that type name, ascending) aliases the rowid.
    if (t.primaryKey.size() == 1 && !t.withoutRowid) {
        const Column& c = t.columns[std::size_t(t.primaryKey[0])];
        if (equalsNoCase(c.declType, "INTEGER") && !t.pkDesc[0])
            t.rowidAlias = t.primaryKey[0];
    }
    if (t.autoincrement && t.rowidAlias < 0)
        invalid("AUTOINCREMENT is only allowed on an INTEGER PRIMARY KEY");
    if (t.withoutRowid && t.primaryKey.empty())
        invalid("PRIMARY KEY missing on table " + s.name);
    // Primary key columns never hold NULL (libsql divergence V5); the rowid
    // alias is assigned when left NULL instead.
    for (const int pk : t.primaryKey) {
        if (pk != t.rowidAlias)
            t.columns[std::size_t(pk)].notNull = true;
        t.columns[std::size_t(pk)].primaryKey = true;
    }
    return t;
}

namespace {

/// The same set of columns as the primary key, under the same collations:
/// uniqueness over a set does not depend on its order, so such a UNIQUE
/// constraint adds nothing the key does not already enforce.
bool sameColumns(const std::vector<IndexPart>& a, const Table& t)
{
    if (a.size() != t.primaryKey.size())
        return false;
    for (const IndexPart& p : a) {
        bool found = false;
        for (std::size_t k = 0; k < t.primaryKey.size(); ++k)
            found = found || (t.primaryKey[k] == p.column && t.pkCollation[k] == p.collation);
        if (!found)
            return false;
    }
    return true;
}

}  // namespace

std::vector<Index> implicitIndexes(const Table& t, const CreateTableStmt& s)
{
    std::vector<Index> out;
    int ordinal = 0;
    // SQLite numbers its autoindexes across the key of a rowid table (which
    // libsql clusters on instead of indexing) and every UNIQUE constraint.
    const bool pkCounts = t.hasRowid() && !t.primaryKey.empty() && t.rowidAlias < 0;
    auto add = [&](std::vector<IndexPart> parts, Conflict conflict) {
        ++ordinal;
        if (sameColumns(parts, t))
            return;
        for (const Index& ix : out) {
            if (ix.parts.size() == parts.size()) {
                bool same = true;
                for (std::size_t i = 0; i < parts.size(); ++i)
                    same = same && ix.parts[i].column == parts[i].column &&
                           ix.parts[i].collation == parts[i].collation;
                if (same)
                    return;
            }
        }
        Index ix;
        ix.name = "sqlite_autoindex_" + t.name + "_" + std::to_string(ordinal);
        ix.table = t.name;
        ix.parts = std::move(parts);
        ix.unique = true;
        ix.onConflict = conflict;
        ix.origin = 1;
        out.push_back(std::move(ix));
    };
    bool pkSeen = false;
    for (const ColumnDef& d : s.columns) {
        if (d.primaryKey && pkCounts) {
            ++ordinal;
            pkSeen = true;
        }
        if (d.unique) {
            const int pos = t.find(d.name);
            IndexPart p;
            p.column = pos;
            p.collation = t.columns[std::size_t(pos)].collation;
            add({p}, d.uniqueConflict);
        }
    }
    for (const TableConstraint& tc : s.constraints) {
        if (tc.kind == TableConstraint::Kind::PrimaryKey && pkCounts && !pkSeen) {
            ++ordinal;
            pkSeen = true;
        }
        if (tc.kind != TableConstraint::Kind::Unique)
            continue;
        std::vector<IndexPart> parts;
        for (const IndexedColumn& ic : tc.columns) {
            IndexPart p;
            p.column = t.find(ic.expr->name);
            p.collation = t.columns[std::size_t(p.column)].collation;
            if (!ic.collation.empty()) {
                const auto c = collationOfName(ic.collation);
                if (!c)
                    invalid("no such collation sequence: " + ic.collation);
                p.collation = *c;
            }
            p.desc = ic.desc;
            parts.push_back(p);
        }
        add(std::move(parts), tc.conflict);
    }
    return out;
}

Index buildIndex(const CreateIndexStmt& s, const Table& t, std::string sql)
{
    Index ix;
    ix.name = s.name;
    ix.table = t.name;
    ix.unique = s.unique;
    ix.sql = std::move(sql);
    const std::string_view src = ix.sql;
    for (const IndexedColumn& ic : s.columns) {
        IndexPart p;
        p.desc = ic.desc;
        if (ic.expr->kind == ExprKind::Column && ic.expr->table.empty()) {
            p.column = t.find(ic.expr->name);
            if (p.column < 0)
                throw Error(ErrorCode::NoSuchColumn, "no such column: " + ic.expr->name);
            if (p.column == int(t.columns.size()))
                p.column = -2;
            else
                p.collation = t.columns[std::size_t(p.column)].collation;
        } else {
            p.column = -1;
            p.exprSql = spanText(src, *ic.expr);
        }
        if (!ic.collation.empty()) {
            const auto c = collationOfName(ic.collation);
            if (!c)
                invalid("no such collation sequence: " + ic.collation);
            p.collation = *c;
        }
        ix.parts.push_back(std::move(p));
    }
    if (s.where)
        ix.whereSql = spanText(src, *s.where);
    return ix;
}

}  // namespace sql::internal
