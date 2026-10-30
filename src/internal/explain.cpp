// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// EXPLAIN QUERY PLAN (and EXPLAIN, which lists the same operators: there is
// no bytecode to show).

#include "internal/stmt.hpp"
#include "internal/util.hpp"

namespace sql::internal {

namespace {

struct Line
{
    int id, parent;
    std::string detail;
};

class Explainer
{
public:
    std::vector<Line> lines;

    int add(int parent, std::string detail)
    {
        const int id = int(lines.size()) + 2;
        lines.push_back({id, parent, std::move(detail)});
        return id;
    }

    std::string access(const SourceNode& src)
    {
        // Covering is settled at the first run; settle it now so the plan
        // shows what that run will do.
        SourceNode& s = const_cast<SourceNode&>(src);
        decideCovering(s);
        const AccessPath& ap = s.access;
        const Table& t = *s.table;
        std::string name = s.alias == t.name ? t.name : t.name + " AS " + s.alias;
        if (ap.kind == AccessPath::Kind::Scan)
            return "SCAN " + name;
        auto colName = [&](int c) {
            return c >= 0 && std::size_t(c) < t.columns.size() ? t.columns[std::size_t(c)].name : std::string("rowid");
        };
        std::vector<std::string> terms;
        std::vector<int> cols;
        if (ap.kind == AccessPath::Kind::Index) {
            for (const IndexPart& p : ap.index->parts)
                cols.push_back(p.column == -2 ? -1 : p.column);
        } else if (ap.kind == AccessPath::Kind::Rowid) {
            cols.push_back(-1);
        } else {
            cols = t.primaryKey;
        }
        std::size_t k = 0;
        for (; k < ap.equals.size() && k < cols.size(); ++k)
            terms.push_back(colName(cols[k]) + "=?");
        if (ap.inList && k < cols.size())
            terms.push_back(colName(cols[k]) + "=?");
        else if ((ap.low || ap.high) && k < cols.size()) {
            if (ap.low)
                terms.push_back(colName(cols[k]) + ">?");
            if (ap.high)
                terms.push_back(colName(cols[k]) + "<?");
        }
        std::string list;
        for (std::size_t i = 0; i < terms.size(); ++i)
            list += (i ? " AND " : "") + terms[i];
        if (ap.kind == AccessPath::Kind::Index) {
            const std::string how = s.coverState == 2 ? " USING COVERING INDEX " : " USING INDEX ";
            if (terms.empty())
                return "SCAN " + name + how + ap.index->name;
            return "SEARCH " + name + how + ap.index->name + " (" + list + ")";
        }
        if (ap.kind == AccessPath::Kind::Rowid)
            return "SEARCH " + name + " USING INTEGER PRIMARY KEY (" + list + ")";
        return "SEARCH " + name + " USING PRIMARY KEY (" + list + ")";
    }

    void subqueries(const Expr* e, int parent)
    {
        if (!e)
            return;
        if (e->sub && e->sub->select) {
            const char* what = e->kind == ExprKind::Exists ? "SUBQUERY" : (e->kind == ExprKind::InSelect ? "LIST SUBQUERY" : "SCALAR SUBQUERY");
            const int id = add(parent, std::string(e->sub->correlated ? "CORRELATED " : "") + what);
            select(*e->sub->select, id);
        }
        subqueries(e->lhs.get(), parent);
        subqueries(e->rhs.get(), parent);
        subqueries(e->extra.get(), parent);
        for (const ExprPtr& x : e->list)
            subqueries(x.get(), parent);
    }

    void core(const CorePlan& c, int parent)
    {
        for (const SourceNode& s : c.sources) {
            switch (s.kind) {
                case SourceNode::Kind::Table: add(parent, access(s)); break;
                case SourceNode::Kind::Rows: {
                    const int id = add(parent, "MATERIALIZE " + s.alias);
                    if (s.rows && s.rows->select && !s.rows->generator)
                        select(*s.rows->select, id);
                    add(parent, "SCAN " + s.alias);
                    break;
                }
                case SourceNode::Kind::Function: add(parent, "SCAN " + s.alias + " VIRTUAL TABLE INDEX 0:"); break;
                case SourceNode::Kind::Recursive: add(parent, "SCAN " + s.alias); break;
                case SourceNode::Kind::Single:
                    if (c.sources.size() == 1 && c.values.empty())
                        add(parent, "SCAN CONSTANT ROW");
                    break;
                case SourceNode::Kind::Pseudo: break;
            }
        }
        for (const Expr* o : c.outputs)
            subqueries(o, parent);
        for (const auto& level : c.levelFilters) {
            for (const Expr* f : level)
                subqueries(f, parent);
        }
        if (c.aggregate && !c.groupBy.empty())
            add(parent, "USE TEMP B-TREE FOR GROUP BY");
        if (c.distinct)
            add(parent, "USE TEMP B-TREE FOR DISTINCT");
    }

    void select(const SelectPlan& sp, int parent)
    {
        if (sp.cores.size() == 1) {
            core(sp.cores[0], parent);
        } else {
            const int id = add(parent, "COMPOUND QUERY");
            for (std::size_t i = 0; i < sp.cores.size(); ++i) {
                std::string label = "LEFT-MOST SUBQUERY";
                if (i > 0) {
                    switch (sp.ops[i - 1]) {
                        case CompoundOp::UnionAll: label = "UNION ALL"; break;
                        case CompoundOp::Union: label = "UNION USING TEMP B-TREE"; break;
                        case CompoundOp::Intersect: label = "INTERSECT USING TEMP B-TREE"; break;
                        case CompoundOp::Except: label = "EXCEPT USING TEMP B-TREE"; break;
                    }
                }
                core(sp.cores[i], add(id, label));
            }
        }
        if (!sp.order.empty())
            add(parent, "USE TEMP B-TREE FOR ORDER BY");
    }
};

}  // namespace

void explainProgram(Program& p, bool queryPlan, ResultBuilder& out)
{
    Explainer ex;
    if (p.select)
        ex.select(*p.select, 0);
    else
        ex.add(0, "EXECUTE " + p.text.substr(0, p.text.find(' ')));
    if (queryPlan) {
        out.columns(std::vector<std::string>{"id", "parent", "notused", "detail"});
        for (const Line& l : ex.lines)
            out.row({Value(std::int64_t(l.id)), Value(std::int64_t(l.parent)), Value(std::int64_t(0)), Value(l.detail)});
        return;
    }
    out.columns(std::vector<std::string>{"addr", "opcode", "p1", "p2", "p3", "p4", "p5", "comment"});
    std::int64_t addr = 0;
    out.row({Value(addr++), Value("Init"), Value(std::int64_t(0)), Value(std::int64_t(0)), Value(std::int64_t(0)),
             Value(), Value(std::int64_t(0)), Value()});
    for (const Line& l : ex.lines)
        out.row({Value(addr++), Value("Plan"), Value(std::int64_t(l.id)), Value(std::int64_t(l.parent)),
                 Value(std::int64_t(0)), Value(l.detail), Value(std::int64_t(0)), Value()});
    out.row({Value(addr++), Value("Halt"), Value(std::int64_t(0)), Value(std::int64_t(0)), Value(std::int64_t(0)),
             Value(), Value(std::int64_t(0)), Value()});
}

}  // namespace sql::internal
