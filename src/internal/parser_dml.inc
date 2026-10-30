// Included inside class Parser (parser.cpp): INSERT, UPDATE, DELETE.

    Conflict conflictAction()
    {
        if (acceptKeyword("ROLLBACK"))
            return Conflict::Rollback;
        if (acceptKeyword("ABORT"))
            return Conflict::Abort;
        if (acceptKeyword("FAIL"))
            return Conflict::Fail;
        if (acceptKeyword("IGNORE"))
            return Conflict::Ignore;
        if (acceptKeyword("REPLACE"))
            return Conflict::Replace;
        fail();
    }

    std::vector<ResultColumn> returningClause()
    {
        std::vector<ResultColumn> out;
        if (!(cur().isKeyword("RETURNING")))
            return out;
        ++at_;
        do {
            ResultColumn rc;
            rc.begin = cur().offset;
            if (acceptPunct("*")) {
                rc.star = true;
            } else {
                rc.expr = expr();
                rc.end = prevEnd();
                if (acceptKeyword("AS"))
                    rc.alias = alias();
                else if (atAlias())
                    rc.alias = alias();
            }
            out.push_back(std::move(rc));
        } while (acceptPunct(","));
        return out;
    }

    std::vector<SetClause> setList()
    {
        std::vector<SetClause> out;
        do {
            SetClause sc;
            if (acceptPunct("(")) {
                do
                    sc.columns.push_back(name());
                while (acceptPunct(","));
                expectPunct(")");
            } else {
                sc.columns.push_back(name());
                // `SET t.a = 1` is not SQLite; a qualified name is an error.
                if (cur().isPunct("."))
                    fail();
            }
            expectPunct("=");
            sc.value = expr();
            out.push_back(std::move(sc));
        } while (acceptPunct(","));
        return out;
    }

    std::vector<IndexedColumn> indexedColumns()
    {
        std::vector<IndexedColumn> out;
        expectPunct("(");
        do {
            IndexedColumn ic;
            ic.begin = cur().offset;
            ExprPtr e = expr();
            // A trailing COLLATE belongs to the indexed column, not the
            // expression.
            if (e->kind == ExprKind::Collate) {
                ic.collation = e->name;
                ExprPtr inner = std::move(e->lhs);
                e = std::move(inner);
            }
            ic.expr = std::move(e);
            ic.end = prevEnd();
            if (acceptKeyword("DESC"))
                ic.desc = true;
            else
                acceptKeyword("ASC");
            out.push_back(std::move(ic));
        } while (acceptPunct(","));
        expectPunct(")");
        return out;
    }

    InsertStmt insert(std::unique_ptr<WithClause> with)
    {
        InsertStmt s;
        s.with = std::move(with);
        if (acceptKeyword("REPLACE")) {
            s.orAction = Conflict::Replace;
        } else {
            expectKeyword("INSERT");
            if (acceptKeyword("OR"))
                s.orAction = conflictAction();
        }
        expectKeyword("INTO");
        qualifiedName(s.schema, s.table);
        if (acceptKeyword("AS"))
            s.alias = name();
        if (acceptPunct("(")) {
            do
                s.columns.push_back(name());
            while (acceptPunct(","));
            expectPunct(")");
        }
        if (cur().isKeyword("DEFAULT")) {
            ++at_;
            expectKeyword("VALUES");
            s.defaultValues = true;
        } else if (cur().isKeyword("VALUES") || cur().isKeyword("SELECT") ||
                   cur().isKeyword("WITH")) {
            s.select = std::make_unique<SelectStmt>(selectStatement());
        } else {
            fail();
        }
        while (cur().isKeyword("ON") && peek().isKeyword("CONFLICT")) {
            at_ += 2;
            Upsert u;
            if (cur().isPunct("(")) {
                u.target = indexedColumns();
                if (acceptKeyword("WHERE"))
                    u.targetWhere = expr();
            }
            expectKeyword("DO");
            if (acceptKeyword("NOTHING")) {
                u.doNothing = true;
            } else {
                expectKeyword("UPDATE");
                expectKeyword("SET");
                u.set = setList();
                if (acceptKeyword("WHERE"))
                    u.where = expr();
            }
            if (!s.upserts.empty() && s.upserts.back().target.empty())
                failWith("unsupported use of ON CONFLICT clause");
            s.upserts.push_back(std::move(u));
        }
        if (s.defaultValues && !s.upserts.empty())
            fail();
        s.returning = returningClause();
        if (s.select)
            attachWith(*s.select, std::move(s.with));
        return s;
    }

    void qualifiedTarget(std::string& schema, std::string& table, std::string& al,
                         std::string& indexedBy, bool& notIndexed)
    {
        qualifiedName(schema, table);
        if (acceptKeyword("AS"))
            al = name();
        if (cur().isKeyword("INDEXED")) {
            ++at_;
            expectKeyword("BY");
            indexedBy = name();
        } else if (cur().isKeyword("NOT") && peek().isKeyword("INDEXED")) {
            at_ += 2;
            notIndexed = true;
        }
    }

    void orderLimit(std::vector<OrderTerm>& order, ExprPtr& limit, ExprPtr& offset)
    {
        if (cur().isKeyword("ORDER") && peek().isKeyword("BY")) {
            at_ += 2;
            order = orderTerms();
        }
        if (acceptKeyword("LIMIT")) {
            limit = expr();
            if (acceptKeyword("OFFSET")) {
                offset = expr();
            } else if (acceptPunct(",")) {
                offset = std::move(limit);
                limit = expr();
            }
        }
        if (!order.empty() && !limit)
            failWith("ORDER BY without LIMIT on UPDATE or DELETE");
    }

    UpdateStmt update(std::unique_ptr<WithClause> with)
    {
        UpdateStmt s;
        s.with = std::move(with);
        expectKeyword("UPDATE");
        if (acceptKeyword("OR"))
            s.orAction = conflictAction();
        qualifiedTarget(s.schema, s.table, s.alias, s.indexedBy, s.notIndexed);
        expectKeyword("SET");
        s.set = setList();
        if (acceptKeyword("FROM"))
            s.from = fromClause();
        if (acceptKeyword("WHERE"))
            s.where = expr();
        s.returning = returningClause();
        orderLimit(s.order, s.limit, s.offset);
        return s;
    }

    DeleteStmt remove(std::unique_ptr<WithClause> with)
    {
        DeleteStmt s;
        s.with = std::move(with);
        expectKeyword("DELETE");
        expectKeyword("FROM");
        qualifiedTarget(s.schema, s.table, s.alias, s.indexedBy, s.notIndexed);
        if (acceptKeyword("WHERE"))
            s.where = expr();
        s.returning = returningClause();
        orderLimit(s.order, s.limit, s.offset);
        return s;
    }
