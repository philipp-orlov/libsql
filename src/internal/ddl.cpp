// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Schema changes. Every object is stored as the CREATE statement that makes
// it (SQLite's sqlite_schema.sql), so ALTER TABLE works by rewriting that
// text and decoding it again.

#include <algorithm>
#include <functional>

#include "internal/dml.hpp"
#include "internal/encoding.hpp"
#include "internal/lexer.hpp"
#include "internal/parser.hpp"
#include "internal/stmt.hpp"
#include "internal/util.hpp"

namespace sql::internal {

namespace {

/// Problems found while planning: the statement cannot run as written.
[[noreturn]] void error(const std::string& what)
{
    throw Error(ErrorCode::InvalidArgument, what);
}

bool reservedName(std::string_view name)
{
    return name.size() >= 7 && equalsNoCase(name.substr(0, 7), "sqlite_");
}

/// The store a DDL statement addresses.
std::size_t storeFor(Session& s, const std::string& schema, bool temp)
{
    if (temp) {
        if (!schema.empty() && !equalsNoCase(schema, "temp"))
            error("temporary table name must be unqualified");
        if (!s.stores[1].catalog)
            s.openTemp(s);
        return 1;
    }
    if (schema.empty())
        return 0;
    const int i = s.storeIndex(schema);
    if (i < 0)
        error("unknown database " + schema);
    if (i == 1 && !s.stores[1].catalog)
        s.openTemp(s);
    return std::size_t(i);
}

/// Any table, index or view of that name in the store (they share a namespace).
std::string objectKind(Catalog& cat, std::string_view name)
{
    if (cat.findTable(name))
        return "table";
    if (cat.findIndex(name))
        return "index";
    if (cat.findView(name))
        return "view";
    return {};
}

std::string quoteIdentifier(std::string_view name)
{
    bool plain = !name.empty() && !(name[0] >= '0' && name[0] <= '9');
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (!((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_'))
            plain = false;
    }
    if (plain) {
        // Keywords that would not read back as names.
        static const char* kWords[] = {"SELECT", "FROM", "WHERE", "TABLE", "INDEX", "ORDER", "GROUP",
                                       "NULL",   "NOT",  "AND",   "OR",    "ON",    "IN",    "IS"};
        for (const char* w : kWords) {
            if (equalsNoCase(name, w))
                plain = false;
        }
    }
    if (plain)
        return std::string(name);
    std::string out = "\"";
    for (const char c : name) {
        out += c;
        if (c == '"')
            out += '"';
    }
    return out + "\"";
}

/// A fresh Program + Writer for running row work inside DDL.
struct Scratch
{
    Program prog;
    std::unique_ptr<Planner> planner;
    std::unique_ptr<Writer> writer;
    Ctx ctx;
    Scratch(Session& s, Ctx& outer)
    {
        planner = std::make_unique<Planner>(prog, s);
        writer = std::make_unique<Writer>(*planner, prog);
        ctx = outer;
    }
    void ready()
    {
        prog.frame.resize(prog.frameSize);
        ctx.slots = prog.frame.data();
    }
};

/// Runs `sql` as a statement in the current transaction (implicit DELETE for
/// DROP TABLE with foreign keys, CREATE TABLE ... AS SELECT's inserts).
std::int64_t runSql(Session& s, Ctx& c, const std::string& sql, const std::vector<Value>* params = nullptr)
{
    Program p;
    p.text = sql;
    p.node = std::make_unique<StatementNode>(parseOne(sql));
    planStatement(p, s, nullptr);
    p.frame.resize(p.frameSize);
    Ctx inner = c;
    inner.slots = p.frame.data();
    static const std::vector<Value> kNone;
    inner.params = params ? params : &kNone;
    inner.run = ++s.conn->runCounter;
    if (p.select) {
        class Null final : public RowSink
        {
        public:
            bool row(Ctx&, const Value*, std::size_t) override { return true; }
        } sink;
        runSelect(*p.select, inner, sink);
        return 0;
    }
    return p.exec->run(inner, nullptr);
}

// ------------------------------------------------------- text rewriting --

/// Replaces tokens of `sql` chosen by `pick` with `replacement`.
std::string rewrite(const std::string& sql, const std::function<bool(const std::vector<Token>&, std::size_t)>& pick,
                    const std::string& replacement)
{
    const std::vector<Token> toks = tokenize(sql);
    std::string out;
    std::size_t at = 0;
    for (std::size_t i = 0; i + 1 < toks.size(); ++i) {
        if (!pick(toks, i))
            continue;
        out.append(sql, at, toks[i].offset - at);
        out += replacement;
        at = toks[i].offset + toks[i].length;
    }
    out.append(sql, at, std::string::npos);
    return out;
}

bool isName(const Token& t, std::string_view name)
{
    return (t.kind == Tok::Identifier || t.kind == Tok::String) &&
           equalsNoCase(t.kind == Tok::String ? t.value.text() : t.text, name);
}

/// Table-name positions: after FROM, JOIN, INTO, UPDATE, ON, TABLE,
/// REFERENCES, a comma inside a FROM list, or as a qualifier before '.'.
std::string renameTableIn(const std::string& sql, std::string_view from, const std::string& to)
{
    return rewrite(
        sql,
        [&](const std::vector<Token>& t, std::size_t i) {
            if (!isName(t[i], from))
                return false;
            if (i + 1 < t.size() && t[i + 1].isPunct("."))
                return true;
            if (i == 0)
                return false;
            const Token& prev = t[i - 1];
            if (prev.isPunct(".")) {
                // schema.table
                return i + 1 >= t.size() || !t[i + 1].isPunct(".");
            }
            static const char* kBefore[] = {"FROM", "JOIN", "INTO", "UPDATE", "ON", "TABLE", "REFERENCES",
                                            "VIEW", "TRIGGER", "EXISTS"};
            for (const char* w : kBefore) {
                if (prev.isKeyword(w))
                    return true;
            }
            return false;
        },
        "\"" + [&] {
            std::string q;
            for (const char ch : to) {
                q += ch;
                if (ch == '"')
                    q += '"';
            }
            return q;
        }() + "\"");
}

}  // namespace

// ---------------------------------------------------------------- plans --

namespace {

struct CreateTablePlan final : ExecPlan
{
    Program* p = nullptr;
    std::int64_t run(Ctx& c, ResultBuilder*) override
    {
        Session& s = *c.session;
        auto& st = std::get<CreateTableStmt>(p->node->body);
        const std::size_t store = storeFor(s, st.schema, st.temp);
        Catalog& cat = s.catalog(store);
        const std::string kind = objectKind(cat, st.name);
        if (!kind.empty()) {
            if (st.ifNotExists)
                return 0;
            throw Error(kind == "table" ? ErrorCode::TableExists : ErrorCode::Runtime,
                        kind + " " + st.name + " already exists");
        }
        if (reservedName(st.name) && !equalsNoCase(st.name, "sqlite_stat1"))
            error("object name reserved for internal use: " + st.name);
        std::string sql;
        std::unique_ptr<Program> source;
        if (st.asSelect) {
            source = std::make_unique<Program>();
            source->text = p->text;
            Planner pl(*source, s);
            source->select = pl.planSelect(*st.asSelect, nullptr, p->text, nullptr);
            source->frame.resize(source->frameSize);
            sql = "CREATE TABLE " + quoteIdentifier(st.name) + "(";
            const SelectPlan& sp = *source->select;
            std::vector<std::string> names;
            for (std::size_t i = 0; i < sp.columns; ++i) {
                std::string n = sp.subNames[i];
                // Duplicate names get :1, :2... as in a sub-query.
                for (int k = 1; std::find_if(names.begin(), names.end(), [&](const std::string& x) {
                                    return equalsNoCase(x, n);
                                }) != names.end();
                     ++k)
                    n = sp.subNames[i] + ":" + std::to_string(k);
                names.push_back(n);
                if (i)
                    sql += ",";
                sql += quoteIdentifier(n);
                if (!sp.declTypes[i].empty())
                    sql += " " + sp.declTypes[i];
            }
            sql += ")";
        } else {
            sql = normalizedCreateSql(p->text, *p->node, st.nameBegin, "CREATE TABLE ");
        }
        const StatementNode node = parseOne(sql);
        const auto& def = std::get<CreateTableStmt>(node.body);
        Table t = buildTable(def, sql);
        std::vector<Index> implicit = implicitIndexes(t, def);
        t.id = cat.newId();
        const std::string name = t.name;
        cat.addTable(std::move(t));
        for (Index& ix : implicit) {
            ix.id = cat.newId();
            cat.addIndex(std::move(ix));
        }
        // Validate CHECK, DEFAULT and generated expressions now, as SQLite does.
        {
            Scratch sc(s, c);
            sc.writer->plan(*cat.findTable(name), store);
        }
        if (source) {
            const std::string insert = "INSERT INTO " + std::string(store == 1 ? "temp." : "") +
                                       quoteIdentifier(name) + " VALUES(";
            Ctx inner = c;
            inner.slots = source->frame.data();
            RowStore rows;
            rows.reset(source->select->columns);
            class Collect final : public RowSink
            {
            public:
                RowStore* r;
                bool row(Ctx&, const Value* v, std::size_t) override
                {
                    r->appendRow(v);
                    return true;
                }
            } sink;
            sink.r = &rows;
            runSelect(*source->select, inner, sink);
            std::string text = insert;
            for (std::size_t i = 0; i < rows.width(); ++i)
                text += i ? ",?" : "?";
            text += ")";
            Program ins;
            ins.text = text;
            ins.node = std::make_unique<StatementNode>(parseOne(text));
            planStatement(ins, s, nullptr);
            ins.frame.resize(ins.frameSize);
            std::vector<Value> params(rows.width());
            for (std::size_t r = 0; r < rows.size(); ++r) {
                for (std::size_t i = 0; i < rows.width(); ++i)
                    params[i] = rows.row(r)[i];
                Ctx ic = c;
                ic.slots = ins.frame.data();
                ic.params = &params;
                ins.exec->run(ic, nullptr);
            }
        }
        return 0;
    }
};

struct CreateIndexPlan final : ExecPlan
{
    Program* p = nullptr;
    std::int64_t run(Ctx& c, ResultBuilder*) override
    {
        Session& s = *c.session;
        auto& st = std::get<CreateIndexStmt>(p->node->body);
        const std::size_t store = storeFor(s, st.schema, false);
        Catalog& cat = s.catalog(store);
        const Table* t = cat.findTable(st.table);
        if (!t) {
            if (cat.findView(st.table))
                error("views may not be indexed");
            throw Error(ErrorCode::NoSuchTable, "no such table: " + std::string(s.stores[store].name) + "." + st.table);
        }
        if (reservedName(t->name) && !equalsNoCase(t->name, "sqlite_stat1"))
            error("table " + t->name + " may not be indexed");
        const std::string kind = objectKind(cat, st.name);
        if (!kind.empty()) {
            if (st.ifNotExists)
                return 0;
            throw Error(kind == "index" ? ErrorCode::IndexExists : ErrorCode::Runtime,
                        kind == "index" ? "index " + st.name + " already exists"
                                        : "there is already a " + kind + " named " + st.name);
        }
        if (reservedName(st.name))
            error("object name reserved for internal use: " + st.name);
        const std::string sql = normalizedCreateSql(
            p->text, *p->node, st.nameBegin, st.unique ? "CREATE UNIQUE INDEX " : "CREATE INDEX ");
        const StatementNode node = parseOne(sql);
        Index ix = buildIndex(std::get<CreateIndexStmt>(node.body), *t, sql);
        ix.id = cat.newId();
        const std::string name = ix.name;
        const std::string table = t->name;
        cat.addIndex(std::move(ix));
        Scratch sc(s, c);
        const Table* nt = cat.findTable(table);
        TablePlan& tp = sc.writer->plan(*nt, store);
        sc.ready();
        sc.writer->backfill(tp, sc.ctx, *cat.findIndex(name));
        return 0;
    }
};

struct CreateViewPlan final : ExecPlan
{
    Program* p = nullptr;
    std::int64_t run(Ctx& c, ResultBuilder*) override
    {
        Session& s = *c.session;
        auto& st = std::get<CreateViewStmt>(p->node->body);
        const std::size_t store = storeFor(s, st.schema, st.temp);
        Catalog& cat = s.catalog(store);
        const std::string kind = objectKind(cat, st.name);
        if (!kind.empty()) {
            if (st.ifNotExists)
                return 0;
            error(kind + " " + st.name + " already exists");
        }
        if (reservedName(st.name))
            error("object name reserved for internal use: " + st.name);
        const std::string sql = normalizedCreateSql(p->text, *p->node, st.nameBegin, "CREATE VIEW ");
        View v;
        v.name = st.name;
        v.sql = sql;
        v.id = cat.newId();
        // The view must plan now.
        {
            Program probe;
            probe.text = sql;
            probe.node = std::make_unique<StatementNode>(parseOne(sql));
            auto& def = std::get<CreateViewStmt>(probe.node->body);
            Planner pl(probe, s);
            auto sp = pl.planSelect(*def.select, nullptr, probe.text, nullptr);
            if (!def.columns.empty() && def.columns.size() != sp->columns)
                error("expected " + std::to_string(def.columns.size()) + " columns for '" + st.name +
                      "' but got " + std::to_string(sp->columns));
        }
        cat.addView(std::move(v));
        return 0;
    }
};

struct CreateTriggerPlan final : ExecPlan
{
    Program* p = nullptr;
    std::int64_t run(Ctx& c, ResultBuilder*) override
    {
        Session& s = *c.session;
        auto& st = std::get<CreateTriggerStmt>(p->node->body);
        std::size_t store = storeFor(s, st.schema, st.temp);
        Catalog* cat = &s.catalog(store);
        if (cat->findTrigger(st.name)) {
            if (st.ifNotExists)
                return 0;
            error("trigger " + st.name + " already exists");
        }
        if (reservedName(st.name))
            error("object name reserved for internal use: " + st.name);
        const Table* t = nullptr;
        const View* v = nullptr;
        for (std::size_t i = 0; i < s.stores.size() && !t && !v; ++i) {
            if (!s.stores[i].catalog)
                continue;
            t = s.catalog(i).findTable(st.table);
            v = t ? nullptr : s.catalog(i).findView(st.table);
            if ((t || v) && !st.temp && st.schema.empty() && i != store) {
                store = i;
                cat = &s.catalog(i);
            }
        }
        if (!t && !v)
            throw Error(ErrorCode::NoSuchTable, "no such table: main." + st.table);
        if (t && reservedName(t->name))
            error("cannot create trigger on system table");
        if (v && st.time != TriggerTime::InsteadOf)
            error(std::string("cannot create ") + (st.time == TriggerTime::Before ? "BEFORE" : "AFTER") +
                  " trigger on view: " + st.table);
        if (t && st.time == TriggerTime::InsteadOf)
            error("cannot create INSTEAD OF trigger on table: " + st.table);
        Trigger tr;
        tr.name = st.name;
        tr.table = t ? t->name : v->name;
        tr.sql = normalizedCreateSql(p->text, *p->node, st.nameBegin, "CREATE TRIGGER ");
        tr.time = st.time;
        tr.event = st.event;
        tr.updateOf = st.updateOf;
        tr.id = cat->newId();
        cat->addTrigger(std::move(tr));
        return 0;
    }
};

struct DropPlan final : ExecPlan
{
    Program* p = nullptr;
    std::int64_t run(Ctx& c, ResultBuilder*) override
    {
        Session& s = *c.session;
        auto& st = std::get<DropStmt>(p->node->body);
        // Unqualified names search temp first, then main and attached stores.
        auto find = [&](auto lookup) -> int {
            if (!st.schema.empty()) {
                const int i = s.storeIndex(st.schema);
                if (i < 0)
                    error("unknown database " + st.schema);
                if (!s.stores[std::size_t(i)].catalog)
                    return -1;
                return lookup(s.catalog(std::size_t(i))) ? i : -1;
            }
            const std::size_t order[] = {1, 0};
            for (const std::size_t i : order) {
                if (i < s.stores.size() && s.stores[i].catalog && lookup(s.catalog(i)))
                    return int(i);
            }
            for (std::size_t i = 2; i < s.stores.size(); ++i) {
                if (s.stores[i].catalog && lookup(s.catalog(i)))
                    return int(i);
            }
            return -1;
        };
        switch (st.kind) {
            case DropStmt::Kind::Table: {
                const int i = find([&](Catalog& cat) { return cat.findTable(st.name) != nullptr; });
                if (i < 0) {
                    if (find([&](Catalog& cat) { return cat.findView(st.name) != nullptr; }) >= 0)
                        error("use DROP VIEW to delete view " + st.name);
                    if (st.ifExists)
                        return 0;
                    throw Error(ErrorCode::NoSuchTable, "no such table: " + st.name);
                }
                Catalog& cat = s.catalog(std::size_t(i));
                const Table* t = cat.findTable(st.name);
                if (reservedName(t->name) && !equalsNoCase(t->name, "sqlite_stat1"))
                    error("table " + t->name + " may not be dropped");
                // With foreign keys on, dropping a parent deletes its rows
                // first so that the children's actions run.
                if (s.conn->foreignKeys) {
                    bool parent = false;
                    for (std::size_t k = 0; k < s.stores.size(); ++k) {
                        if (s.stores[k].catalog && !s.catalog(k).schema().childrenOf(t->name).empty())
                            parent = true;
                    }
                    if (parent) {
                        runSql(s, c, "DELETE FROM " + std::string(s.stores[std::size_t(i)].name) + "." + quoteIdentifier(t->name));
                        if (s.immediateFkViolations > 0) {
                            s.immediateFkViolations = 0;
                            throw Error(ErrorCode::ConstraintViolation, Constraint::ForeignKey,
                                        "FOREIGN KEY constraint failed");
                        }
                    }
                }
                cat.dropTable(st.name);
                return 0;
            }
            case DropStmt::Kind::Index: {
                const int i = find([&](Catalog& cat) { return cat.findIndex(st.name) != nullptr; });
                if (i < 0) {
                    if (st.ifExists)
                        return 0;
                    throw Error(ErrorCode::NoSuchIndex, "no such index: " + st.name);
                }
                Catalog& cat = s.catalog(std::size_t(i));
                if (cat.findIndex(st.name)->origin != 0)
                    error("index associated with UNIQUE or PRIMARY KEY constraint cannot be dropped");
                cat.dropIndex(st.name);
                return 0;
            }
            case DropStmt::Kind::View: {
                const int i = find([&](Catalog& cat) { return cat.findView(st.name) != nullptr; });
                if (i < 0) {
                    if (find([&](Catalog& cat) { return cat.findTable(st.name) != nullptr; }) >= 0)
                        error("use DROP TABLE to delete table " + st.name);
                    if (st.ifExists)
                        return 0;
                    error("no such view: " + st.name);
                }
                Catalog& cat = s.catalog(std::size_t(i));
                // Triggers on the view go with it.
                std::vector<std::string> doomed;
                for (const auto& tr : cat.schema().triggers) {
                    if (equalsNoCase(tr->table, st.name))
                        doomed.push_back(tr->name);
                }
                for (const std::string& n : doomed)
                    cat.dropTrigger(n);
                cat.dropView(st.name);
                return 0;
            }
            case DropStmt::Kind::Trigger: {
                const int i = find([&](Catalog& cat) { return cat.findTrigger(st.name) != nullptr; });
                if (i < 0) {
                    if (st.ifExists)
                        return 0;
                    error("no such trigger: " + st.name);
                }
                s.catalog(std::size_t(i)).dropTrigger(st.name);
                return 0;
            }
        }
        return 0;
    }
};

/// The span of the column list's closing parenthesis in a CREATE TABLE text.
std::size_t closingParen(const std::string& sql)
{
    const std::vector<Token> toks = tokenize(sql);
    int depth = 0;
    for (const Token& t : toks) {
        if (t.isPunct("(")) {
            ++depth;
        } else if (t.isPunct(")")) {
            if (--depth == 0)
                return t.offset;
        }
    }
    error("malformed table definition");
}

struct AlterPlan final : ExecPlan
{
    Program* p = nullptr;

    /// Every view must still plan after a change.
    static void checkViews(Session& s)
    {
        for (std::size_t i = 0; i < s.stores.size(); ++i) {
            if (!s.stores[i].catalog)
                continue;
            for (const auto& v : s.catalog(i).schema().views) {
                Program probe;
                probe.text = v->sql;
                probe.node = std::make_unique<StatementNode>(parseOne(v->sql));
                auto& def = std::get<CreateViewStmt>(probe.node->body);
                Planner pl(probe, s);
                try {
                    pl.planSelect(*def.select, nullptr, probe.text, nullptr);
                } catch (const Error& e) {
                    error(std::string("error in view ") + v->name + ": " + e.what());
                }
            }
        }
    }

    std::int64_t run(Ctx& c, ResultBuilder*) override
    {
        Session& s = *c.session;
        auto& st = std::get<AlterTableStmt>(p->node->body);
        int store = -1;
        for (std::size_t i = 0; i < s.stores.size() && store < 0; ++i) {
            if (!s.stores[i].catalog)
                continue;
            if (!st.schema.empty() && !equalsNoCase(s.stores[i].name, st.schema))
                continue;
            if (s.catalog(i).findTable(st.table))
                store = int(i);
        }
        if (store < 0)
            throw Error(ErrorCode::NoSuchTable, "no such table: " + st.table);
        Catalog& cat = s.catalog(std::size_t(store));
        const Table& t = *cat.findTable(st.table);
        if (reservedName(t.name))
            error("table " + t.name + " may not be altered");
        const std::string tableName = t.name;
        switch (st.action) {
            case AlterTableStmt::Action::RenameTable: {
                if (!objectKind(cat, st.newName).empty())
                    error("there is already another table or index with this name: " + st.newName);
                if (reservedName(st.newName))
                    error("object name reserved for internal use: " + st.newName);
                // The table itself.
                Table nt = buildTable(
                    std::get<CreateTableStmt>(parseOne(renameTableIn(t.sql, tableName, st.newName)).body),
                    renameTableIn(t.sql, tableName, st.newName));
                const std::string newSql = renameTableIn(t.sql, tableName, st.newName);
                {
                    const StatementNode node = parseOne(newSql);
                    nt = buildTable(std::get<CreateTableStmt>(node.body), newSql);
                }
                nt.name = st.newName;
                cat.replaceTable(tableName, std::move(nt));
                // Its indexes (implicit ones are renamed as SQLite does).
                std::vector<std::pair<std::string, Index>> idx;
                for (const auto& ix : cat.schema().indexes) {
                    if (!equalsNoCase(ix->table, tableName))
                        continue;
                    Index n = *ix;
                    n.table = st.newName;
                    if (!n.sql.empty())
                        n.sql = renameTableIn(n.sql, tableName, st.newName);
                    else if (n.name.size() > 17)
                        n.name = "sqlite_autoindex_" + st.newName + n.name.substr(n.name.rfind('_'));
                    idx.emplace_back(ix->name, std::move(n));
                }
                for (auto& [old, n] : idx)
                    cat.replaceIndex(old, std::move(n));
                // Foreign keys, triggers and views that mention it.
                for (const auto& other : std::vector<const Table*>([&] {
                         std::vector<const Table*> v;
                         for (const auto& x : cat.schema().tables)
                             v.push_back(x.get());
                         return v;
                     }())) {
                    bool refs = false;
                    for (const ForeignKey& fk : other->foreignKeys)
                        refs = refs || equalsNoCase(fk.parentTable, tableName);
                    if (!refs)
                        continue;
                    const std::string sql = renameTableIn(other->sql, tableName, st.newName);
                    const StatementNode node = parseOne(sql);
                    Table rt = buildTable(std::get<CreateTableStmt>(node.body), sql);
                    rt.name = other->name;
                    const std::string oldName = other->name;
                    cat.replaceTable(oldName, std::move(rt));
                }
                std::vector<std::pair<std::string, Trigger>> trs;
                for (const auto& tr : cat.schema().triggers) {
                    Trigger n = *tr;
                    n.sql = renameTableIn(n.sql, tableName, st.newName);
                    if (equalsNoCase(n.table, tableName))
                        n.table = st.newName;
                    if (n.sql != tr->sql || n.table != tr->table)
                        trs.emplace_back(tr->name, std::move(n));
                }
                for (auto& [old, n] : trs)
                    cat.replaceTrigger(old, std::move(n));
                std::vector<std::pair<std::string, View>> vs;
                for (const auto& v : cat.schema().views) {
                    View n = *v;
                    n.sql = renameTableIn(n.sql, tableName, st.newName);
                    if (n.sql != v->sql)
                        vs.emplace_back(v->name, std::move(n));
                }
                for (auto& [old, n] : vs)
                    cat.replaceView(old, std::move(n));
                checkViews(s);
                return 0;
            }
            case AlterTableStmt::Action::RenameColumn: {
                const int col = t.find(st.column);
                if (col < 0 || col >= int(t.columns.size()))
                    error("no such column: \"" + st.column + "\"");
                if (t.find(st.newName) >= 0 && t.find(st.newName) < int(t.columns.size()))
                    error("duplicate column name: " + st.newName);
                const std::string oldCol = t.columns[std::size_t(col)].name;
                const std::string qn = quoteIdentifier(st.newName);
                // In the table's own text every bare mention is the column,
                // except the table name itself.
                const std::string tsql = rewrite(
                    t.sql,
                    [&](const std::vector<Token>& tk, std::size_t i) {
                        if (!isName(tk[i], oldCol))
                            return false;
                        if (i >= 2 && tk[i - 1].isKeyword("TABLE"))
                            return false;
                        if (i + 1 < tk.size() && tk[i + 1].isPunct("."))
                            return false;
                        if (i >= 1 && tk[i - 1].isKeyword("REFERENCES"))
                            return false;
                        return true;
                    },
                    qn);
                {
                    const StatementNode node = parseOne(tsql);
                    Table nt = buildTable(std::get<CreateTableStmt>(node.body), tsql);
                    nt.name = tableName;
                    cat.replaceTable(tableName, std::move(nt));
                }
                auto inTableContext = [&](const std::string& sql) {
                    return rewrite(
                        sql,
                        [&](const std::vector<Token>& tk, std::size_t i) {
                            if (!isName(tk[i], oldCol))
                                return false;
                            if (i >= 2 && tk[i - 1].isPunct(".")) {
                                const Token& q = tk[i - 2];
                                return isName(q, tableName) || q.isKeyword("new") || q.isKeyword("old");
                            }
                            return !(i + 1 < tk.size() && tk[i + 1].isPunct("."));
                        },
                        qn);
                };
                std::vector<std::pair<std::string, Index>> idx;
                for (const auto& ix : cat.schema().indexes) {
                    if (!equalsNoCase(ix->table, tableName) || ix->sql.empty())
                        continue;
                    Index n = *ix;
                    n.sql = inTableContext(ix->sql);
                    const StatementNode node = parseOne(n.sql);
                    Index rebuilt = buildIndex(std::get<CreateIndexStmt>(node.body), *cat.findTable(tableName), n.sql);
                    rebuilt.name = ix->name;
                    idx.emplace_back(ix->name, std::move(rebuilt));
                }
                for (auto& [old, n] : idx)
                    cat.replaceIndex(old, std::move(n));
                // Implicit indexes refer to columns by position: rebuild.
                cat.replaceTable(tableName, Table(*cat.findTable(tableName)));
                std::vector<std::pair<std::string, Trigger>> trs;
                for (const auto& tr : cat.schema().triggers) {
                    if (!equalsNoCase(tr->table, tableName))
                        continue;
                    Trigger n = *tr;
                    n.sql = inTableContext(tr->sql);
                    for (std::string& u : n.updateOf) {
                        if (equalsNoCase(u, oldCol))
                            u = st.newName;
                    }
                    trs.emplace_back(tr->name, std::move(n));
                }
                for (auto& [old, n] : trs)
                    cat.replaceTrigger(old, std::move(n));
                // Children whose foreign keys name the column.
                std::vector<const Table*> kids;
                for (const auto& x : cat.schema().tables)
                    kids.push_back(x.get());
                for (const Table* other : kids) {
                    bool refs = false;
                    for (const ForeignKey& fk : other->foreignKeys)
                        refs = refs || equalsNoCase(fk.parentTable, tableName);
                    if (!refs)
                        continue;
                    std::string sql = rewrite(
                        other->sql,
                        [&](const std::vector<Token>& tk, std::size_t i) {
                            if (!isName(tk[i], oldCol))
                                return false;
                            // Inside REFERENCES parent(...).
                            for (std::size_t k = i; k-- > 0;) {
                                if (tk[k].isPunct(")"))
                                    return false;
                                if (tk[k].isPunct("(")) {
                                    return k >= 2 && isName(tk[k - 1], tableName) &&
                                           tk[k - 2].isKeyword("REFERENCES");
                                }
                            }
                            return false;
                        },
                        qn);
                    if (sql == other->sql)
                        continue;
                    const StatementNode node = parseOne(sql);
                    Table rt = buildTable(std::get<CreateTableStmt>(node.body), sql);
                    rt.name = other->name;
                    const std::string oldName = other->name;
                    cat.replaceTable(oldName, std::move(rt));
                }
                checkViews(s);
                return 0;
            }
            case AlterTableStmt::Action::AddColumn: {
                const ColumnDef& d = st.def;
                if (t.find(d.name) >= 0 && t.find(d.name) < int(t.columns.size()))
                    error("duplicate column name: " + d.name);
                if (d.primaryKey)
                    error("Cannot add a PRIMARY KEY column");
                if (d.unique)
                    error("Cannot add a UNIQUE column");
                if (d.generated && d.stored)
                    error("cannot add a STORED column");
                if (d.defaultExpr && d.defaultExpr->kind != ExprKind::Literal &&
                    !(d.defaultExpr->kind == ExprKind::Unary))
                    error("Cannot add a column with non-constant default");
                const bool nullDefault = !d.defaultExpr || (d.defaultExpr->kind == ExprKind::Literal &&
                                                            d.defaultExpr->literal.isNull());
                if (d.notNull && nullDefault && !d.generated)
                    error("Cannot add a NOT NULL column with default value NULL");
                if (!d.references.empty() && s.conn->foreignKeys && !nullDefault)
                    error("Cannot add a REFERENCES column with non-NULL default value");
                const std::string def = p->text.substr(st.defBegin, st.defEnd - st.defBegin);
                std::string sql = t.sql;
                const std::size_t at = closingParen(sql);
                sql.insert(at, ", " + def);
                const StatementNode node = parseOne(sql);
                Table nt = buildTable(std::get<CreateTableStmt>(node.body), sql);
                nt.name = tableName;
                cat.replaceTable(tableName, std::move(nt));
                // Existing rows must satisfy a CHECK on the new column.
                if (!d.checks.empty() || (d.generated && d.notNull)) {
                    Program probe;
                    std::string q = "SELECT 1 FROM " + std::string(s.stores[std::size_t(store)].name) + "." +
                                    quoteIdentifier(tableName) + " WHERE NOT (";
                    bool first = true;
                    for (const CheckDef& ck : d.checks) {
                        q += (first ? "" : " AND ") + std::string("(") +
                             def.substr(ck.expr->begin - st.defBegin, ck.expr->end - ck.expr->begin) + ")";
                        first = false;
                    }
                    if (first)
                        q += quoteIdentifier(d.name) + " IS NOT NULL";
                    q += ") LIMIT 1";
                    probe.text = q;
                    probe.node = std::make_unique<StatementNode>(parseOne(q));
                    planStatement(probe, s, nullptr);
                    Ctx pc = c;
                    pc.slots = probe.frame.data();
                    pc.run = ++s.conn->runCounter;
                    class Any final : public RowSink
                    {
                    public:
                        bool hit = false;
                        bool row(Ctx&, const Value*, std::size_t) override
                        {
                            hit = true;
                            return false;
                        }
                    } sink;
                    runSelect(*probe.select, pc, sink);
                    if (sink.hit)
                        throw Error(ErrorCode::ConstraintViolation, Constraint::Check,
                                    d.checks.empty() ? "NOT NULL constraint failed" : "CHECK constraint failed");
                }
                return 0;
            }
            case AlterTableStmt::Action::DropColumn: {
                const int col = t.find(st.column);
                if (col < 0 || col >= int(t.columns.size()))
                    error("no such column: \"" + st.column + "\"");
                const std::string cn = t.columns[std::size_t(col)].name;
                const std::string why = "cannot drop column \"" + cn + "\": ";
                if (t.columns.size() == 1)
                    error(why + "no other columns exist");
                if (t.columns[std::size_t(col)].primaryKey)
                    error(why + "PRIMARY KEY");
                for (const Index* ix : t.indexes) {
                    for (const IndexPart& part : ix->parts) {
                        if (part.column == col)
                            error(why + (ix->origin ? "UNIQUE" : "indexed"));
                    }
                    if (!ix->whereSql.empty() || ix->hasExpressions()) {
                        for (const Token& tk : tokenize(ix->whereSql + " " + ix->sql)) {
                            if (isName(tk, cn))
                                error(why + "indexed");
                        }
                    }
                }
                for (const ForeignKey& fk : t.foreignKeys) {
                    if (std::find(fk.columns.begin(), fk.columns.end(), col) != fk.columns.end())
                        error(why + "foreign key");
                }
                for (const Column& other : t.columns) {
                    if (!other.generated)
                        continue;
                    for (const Token& tk : tokenize(other.generatedSql)) {
                        if (isName(tk, cn))
                            error(why + "generated column");
                    }
                }
                // Remove the column definition from the text.
                const std::vector<Token> tk = tokenize(t.sql);
                int depth = 0;
                std::size_t start = 0, stop = 0;
                bool atItemStart = false;
                for (std::size_t i = 0; i + 1 < tk.size(); ++i) {
                    if (tk[i].isPunct("(")) {
                        if (++depth == 1)
                            atItemStart = true;
                        continue;
                    }
                    if (tk[i].isPunct(")")) {
                        if (--depth == 0 && start) {
                            stop = tk[i].offset;
                            break;
                        }
                        continue;
                    }
                    if (depth == 1 && tk[i].isPunct(",")) {
                        if (start) {
                            stop = tk[i].offset;
                            break;
                        }
                        atItemStart = true;
                        continue;
                    }
                    if (depth == 1 && atItemStart) {
                        atItemStart = false;
                        if (isName(tk[i], cn) && !start)
                            start = tk[i].offset;
                    }
                }
                if (!start)
                    error("malformed table definition");
                std::string sql = t.sql;
                // Remove ", def" (or "def, " when it is the first column).
                std::size_t from = start, to = stop;
                std::size_t comma = sql.rfind(',', start);
                const std::size_t open = sql.find('(');
                if (comma != std::string::npos && comma > open)
                    from = comma;
                else if (sql[to] == ',')
                    to = to + 1;
                sql.erase(from, to - from);
                const StatementNode node = parseOne(sql);
                Table nt = buildTable(std::get<CreateTableStmt>(node.body), sql);
                nt.name = tableName;
                // CHECK constraints may not mention the dropped column.
                for (const Check& ck : nt.checks) {
                    for (const Token& x : tokenize(ck.sql)) {
                        if (isName(x, cn))
                            error(why + "CHECK constraint");
                    }
                }
                // Rewrite every row without the column.
                const nosql::Db& rows = cat.rows(t);
                std::vector<std::pair<std::string, std::string>> rewritten;
                {
                    nosql::Cursor cur = rows.cursor();
                    std::vector<Value> vals(t.columns.size());
                    for (bool ok = cur.first(); ok; ok = cur.next()) {
                        std::string_view payload = viewOf(cur.value());
                        std::string out;
                        if (t.payloadRowid())
                            putVarint(out, getVarint(payload));
                        const std::size_t have = decodeRowInto(payload, vals.data(), vals.size());
                        for (std::size_t i = have; i < vals.size(); ++i) {
                            const Column& cc = t.columns[i];
                            vals[i] = cc.hasDefault && cc.defaultConstant ? cc.defaultValue : Value();
                        }
                        putVarint(out, vals.size() - 1);
                        for (std::size_t i = 0; i < vals.size(); ++i) {
                            if (int(i) != col)
                                putValue(out, vals[i]);
                        }
                        rewritten.emplace_back(std::string(viewOf(cur.key())), std::move(out));
                    }
                }
                for (auto& [k, v] : rewritten)
                    rows.put(sliceOf(k), sliceOf(v));
                cat.replaceTable(tableName, std::move(nt));
                checkViews(s);
                return 0;
            }
        }
        return 0;
    }
};

struct AnalyzePlan final : ExecPlan
{
    Program* p = nullptr;
    std::int64_t run(Ctx& c, ResultBuilder*) override
    {
        Session& s = *c.session;
        auto& st = std::get<AnalyzeStmt>(p->node->body);
        for (std::size_t i = 0; i < s.stores.size(); ++i) {
            if (!s.stores[i].catalog)
                continue;
            Catalog& cat = s.catalog(i);
            std::vector<const Table*> targets;
            for (const auto& t : cat.schema().tables) {
                if (reservedName(t->name))
                    continue;
                if (!st.name.empty()) {
                    const bool schemaOnly = st.schema.empty() && s.storeIndex(st.name) == int(i);
                    if (!schemaOnly) {
                        if (!st.schema.empty() && !equalsNoCase(s.stores[i].name, st.schema))
                            continue;
                        const Index* ix = cat.findIndex(st.name);
                        if (!equalsNoCase(t->name, st.name) && !(ix && equalsNoCase(ix->table, t->name)))
                            continue;
                    }
                }
                targets.push_back(t.get());
            }
            if (targets.empty())
                continue;
            const std::string prefix = std::string(s.stores[i].name) + ".";
            if (!cat.findTable("sqlite_stat1")) {
                // Creating the table replaces the schema: look the targets up again.
                std::vector<std::string> names;
                for (const Table* t : targets)
                    names.push_back(t->name);
                runSql(s, c, "CREATE TABLE " + prefix + "sqlite_stat1(tbl,idx,stat)");
                for (std::size_t k = 0; k < names.size(); ++k)
                    targets[k] = cat.findTable(names[k]);
            }
            std::vector<std::vector<Value>> stats;
            for (const Table* t : targets) {
                const std::uint64_t n = cat.rows(*t).count();
                std::vector<Value> row{Value(t->name), Value(), Value(std::to_string(n))};
                if (t->indexes.empty()) {
                    stats.push_back(row);
                    continue;
                }
                for (const Index* ix : t->indexes) {
                    // Average rows per distinct prefix of 1..k parts.
                    const std::size_t k = ix->parts.size();
                    std::vector<std::uint64_t> distinct(k, 0);
                    std::vector<std::string> last(k);
                    std::uint64_t total = 0;
                    nosql::Cursor cur = cat.entries(*ix).cursor();
                    for (bool ok = cur.first(); ok; ok = cur.next()) {
                        ++total;
                        std::string_view key = viewOf(cur.key());
                        std::string_view rest = key;
                        bool changed = false;
                        for (std::size_t j = 0; j < k; ++j) {
                            skipKeys(rest, 1);
                            const std::string_view pre = key.substr(0, key.size() - rest.size());
                            if (changed || pre != last[j]) {
                                changed = true;
                                ++distinct[j];
                                last[j].assign(pre.data(), pre.size());
                            }
                        }
                    }
                    std::string stat = std::to_string(total);
                    for (std::size_t j = 0; j < k; ++j)
                        stat += " " + std::to_string(distinct[j] ? (total + distinct[j] - 1) / distinct[j] : 1);
                    stats.push_back({Value(t->name), Value(ix->name), Value(stat)});
                }
            }
            for (const Table* t : targets) {
                const std::vector<Value> params{Value(t->name)};
                runSql(s, c, "DELETE FROM " + prefix + "sqlite_stat1 WHERE tbl = ?", &params);
            }
            for (const std::vector<Value>& row : stats)
                runSql(s, c, "INSERT INTO " + prefix + "sqlite_stat1 VALUES(?,?,?)", &row);
        }
        return 0;
    }
};

struct ReindexPlan final : ExecPlan
{
    Program* p = nullptr;
    std::int64_t run(Ctx& c, ResultBuilder*) override
    {
        Session& s = *c.session;
        auto& st = std::get<ReindexStmt>(p->node->body);
        const auto coll = collationOfName(st.name);
        bool any = st.name.empty();
        for (std::size_t i = 0; i < s.stores.size(); ++i) {
            if (!s.stores[i].catalog)
                continue;
            if (!st.schema.empty() && !equalsNoCase(s.stores[i].name, st.schema))
                continue;
            Catalog& cat = s.catalog(i);
            std::vector<std::pair<std::string, std::string>> todo;  // table, index
            for (const auto& ix : cat.schema().indexes) {
                bool hit = st.name.empty() || equalsNoCase(ix->name, st.name) || equalsNoCase(ix->table, st.name);
                if (coll) {
                    for (const IndexPart& part : ix->parts)
                        hit = hit || part.collation == *coll;
                }
                if (hit)
                    todo.emplace_back(ix->table, ix->name);
            }
            if (!todo.empty() || (!st.name.empty() && (cat.findTable(st.name) || cat.findIndex(st.name))))
                any = true;
            for (const auto& [table, index] : todo) {
                const Index* ix = cat.findIndex(index);
                cat.entries(*ix).clear();
                Scratch sc(s, c);
                TablePlan& tp = sc.writer->plan(*cat.findTable(table), i);
                sc.ready();
                sc.writer->backfill(tp, sc.ctx, *ix);
            }
        }
        if (!any && !coll)
            error("unable to identify the object to be reindexed");
        return 0;
    }
};

}  // namespace

std::unique_ptr<ExecPlan> planDdl(Program& p, Session& s)
{
    (void)s;
    StatementBody& body = p.node->body;
    if (std::holds_alternative<CreateTableStmt>(body)) {
        auto plan = std::make_unique<CreateTablePlan>();
        plan->p = &p;
        return plan;
    }
    if (std::holds_alternative<CreateIndexStmt>(body)) {
        auto plan = std::make_unique<CreateIndexPlan>();
        plan->p = &p;
        return plan;
    }
    if (std::holds_alternative<CreateViewStmt>(body)) {
        auto plan = std::make_unique<CreateViewPlan>();
        plan->p = &p;
        return plan;
    }
    if (std::holds_alternative<CreateTriggerStmt>(body)) {
        auto plan = std::make_unique<CreateTriggerPlan>();
        plan->p = &p;
        return plan;
    }
    if (std::holds_alternative<DropStmt>(body)) {
        auto plan = std::make_unique<DropPlan>();
        plan->p = &p;
        return plan;
    }
    if (std::holds_alternative<AlterTableStmt>(body)) {
        auto plan = std::make_unique<AlterPlan>();
        plan->p = &p;
        return plan;
    }
    if (std::holds_alternative<AnalyzeStmt>(body)) {
        auto plan = std::make_unique<AnalyzePlan>();
        plan->p = &p;
        return plan;
    }
    if (std::holds_alternative<ReindexStmt>(body)) {
        auto plan = std::make_unique<ReindexPlan>();
        plan->p = &p;
        return plan;
    }
    throw Error(ErrorCode::Unsupported, "statement not supported here");
}

}  // namespace sql::internal
