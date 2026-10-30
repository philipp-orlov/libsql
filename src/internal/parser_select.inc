// Included inside class Parser (parser.cpp): SELECT and its clauses.

    /// May the current token be read as an alias? Quoted names and strings
    /// always may; a bare word may unless it is reserved or starts a clause.
    bool atAlias() const
    {
        const Token& t = cur();
        if (t.kind == Tok::String)
            return true;
        if (t.kind != Tok::Identifier)
            return false;
        if (t.quoted)
            return true;
        if (t.reserved || t.joinWord || t.isKeyword("INDEXED"))
            return false;
        if (t.isKeyword("FOR") && peek().isKeyword("JSON"))
            return false;
        if (t.isKeyword("WINDOW") && peek().kind == Tok::Identifier && peek(2).isKeyword("AS"))
            return false;
        if (t.isKeyword("RETURNING"))
            return false;
        return true;
    }

    std::string alias()
    {
        const Token& t = take();
        return t.kind == Tok::String ? std::string(t.value.text()) : t.text;
    }

    SelectStmt selectStatement()
    {
        std::unique_ptr<WithClause> with;
        if (cur().isKeyword("WITH"))
            with = withClause();
        return selectAfterWith(std::move(with));
    }

    SelectStmt selectAfterWith(std::unique_ptr<WithClause> with)
    {
        enter();
        SelectStmt s;
        s.with = std::move(with);
        s.cores.push_back(selectCore());
        for (;;) {
            CompoundOp op;
            if (cur().isKeyword("UNION")) {
                ++at_;
                op = acceptKeyword("ALL") ? CompoundOp::UnionAll : CompoundOp::Union;
            } else if (acceptKeyword("INTERSECT")) {
                op = CompoundOp::Intersect;
            } else if (acceptKeyword("EXCEPT")) {
                op = CompoundOp::Except;
            } else {
                break;
            }
            if (s.cores.size() >= kMaxCompound)
                failWith("too many terms in compound SELECT");
            s.ops.push_back(op);
            s.cores.push_back(selectCore());
        }
        if (cur().isKeyword("ORDER") && peek().isKeyword("BY")) {
            at_ += 2;
            s.order = orderTerms();
        }
        if (acceptKeyword("LIMIT")) {
            s.limit = expr();
            if (acceptKeyword("OFFSET")) {
                s.offset = expr();
            } else if (acceptPunct(",")) {
                s.offset = std::move(s.limit);
                s.limit = expr();
            }
        }
        if (cur().isKeyword("FOR") && peek().isKeyword("JSON")) {
            at_ += 2;
            s.forJson = forJson();
        }
        leave();
        return s;
    }

    /// `FOR JSON {AUTO|PATH} [, ROOT[('name')]] [, INCLUDE_NULL_VALUES]
    /// [, WITHOUT_ARRAY_WRAPPER]`, with `FOR JSON` already consumed.
    JsonOptions forJson()
    {
        JsonOptions options;
        if (acceptKeyword("AUTO"))
            options.mode(JsonMode::Auto);
        else if (acceptKeyword("PATH"))
            options.mode(JsonMode::Path);
        else
            failWith("expected AUTO or PATH after FOR JSON");

        while (acceptPunct(",")) {
            if (acceptKeyword("ROOT")) {
                std::string nm;
                if (acceptPunct("(")) {
                    if (cur().kind != Tok::String)
                        failWith("ROOT needs a quoted name");
                    nm = take().value.text();
                    expectPunct(")");
                }
                options.root(nm);
            } else if (acceptKeyword("INCLUDE_NULL_VALUES")) {
                options.includeNullValues();
            } else if (acceptKeyword("WITHOUT_ARRAY_WRAPPER")) {
                options.withoutArrayWrapper();
            } else {
                failWith("expected ROOT, INCLUDE_NULL_VALUES or WITHOUT_ARRAY_WRAPPER");
            }
        }
        if (options.hasRoot() && options.omitsArrayWrapper())
            throw Error(ErrorCode::InvalidArgument,
                        "ROOT and WITHOUT_ARRAY_WRAPPER cannot be combined");
        return options;
    }

    std::vector<OrderTerm> orderTerms()
    {
        std::vector<OrderTerm> out;
        do {
            OrderTerm term;
            term.expr = expr();
            if (acceptKeyword("DESC"))
                term.desc = true;
            else
                acceptKeyword("ASC");
            if (acceptKeyword("NULLS")) {
                if (acceptKeyword("FIRST"))
                    term.nulls = 1;
                else if (acceptKeyword("LAST"))
                    term.nulls = 2;
                else
                    fail();
            }
            out.push_back(std::move(term));
        } while (acceptPunct(","));
        return out;
    }

    SelectCore selectCore()
    {
        SelectCore core;
        if (acceptKeyword("VALUES")) {
            do {
                expectPunct("(");
                std::vector<ExprPtr> row;
                do
                    row.push_back(expr());
                while (acceptPunct(","));
                expectPunct(")");
                if (!core.values.empty() && row.size() != core.values.front().size())
                    failWith("all VALUES must have the same number of terms");
                core.values.push_back(std::move(row));
            } while (acceptPunct(","));
            return core;
        }
        expectKeyword("SELECT");
        if (acceptKeyword("DISTINCT"))
            core.distinct = true;
        else
            acceptKeyword("ALL");
        do
            core.columns.push_back(resultColumn());
        while (acceptPunct(","));
        if (acceptKeyword("FROM"))
            core.from = fromClause();
        if (acceptKeyword("WHERE"))
            core.where = expr();
        if (cur().isKeyword("GROUP")) {
            ++at_;
            expectKeyword("BY");
            do
                core.groupBy.push_back(expr());
            while (acceptPunct(","));
        }
        if (acceptKeyword("HAVING"))
            core.having = expr();
        if (cur().isKeyword("WINDOW") && peek().kind == Tok::Identifier) {
            ++at_;
            do {
                NamedWindow w;
                w.name = name();
                expectKeyword("AS");
                w.spec = windowDefinition();
                core.windows.push_back(std::move(w));
            } while (acceptPunct(","));
        }
        return core;
    }

    ResultColumn resultColumn()
    {
        ResultColumn rc;
        rc.begin = cur().offset;
        if (acceptPunct("*")) {
            rc.star = true;
            return rc;
        }
        if (cur().kind == Tok::Identifier && peek().isPunct(".") && peek(2).isPunct("*")) {
            rc.star = true;
            rc.starTable = take().text;
            at_ += 2;
            return rc;
        }
        if (cur().kind == Tok::Identifier && peek().isPunct(".") &&
            peek(2).kind == Tok::Identifier && peek(3).isPunct(".") && peek(4).isPunct("*")) {
            // schema.table.*
            at_ += 2;
            rc.star = true;
            rc.starTable = take().text;
            at_ += 2;
            return rc;
        }
        rc.expr = expr();
        rc.end = prevEnd();
        if (acceptKeyword("AS")) {
            if (cur().kind == Tok::String || cur().kind == Tok::Identifier)
                rc.alias = alias();
            else
                fail();
        } else if (atAlias()) {
            rc.alias = alias();
        }
        return rc;
    }

    std::vector<FromItem> fromClause()
    {
        std::vector<FromItem> items;
        items.push_back(fromItem());
        for (;;) {
            JoinType join = JoinType::Inner;
            bool natural = false;
            if (acceptPunct(",")) {
                join = JoinType::Inner;
            } else {
                const std::size_t mark = at_;
                if (acceptKeyword("NATURAL"))
                    natural = true;
                if (acceptKeyword("LEFT")) {
                    acceptKeyword("OUTER");
                    join = JoinType::Left;
                } else if (acceptKeyword("RIGHT")) {
                    acceptKeyword("OUTER");
                    join = JoinType::Right;
                } else if (acceptKeyword("FULL")) {
                    acceptKeyword("OUTER");
                    join = JoinType::Full;
                } else if (acceptKeyword("INNER")) {
                    join = JoinType::Inner;
                } else if (acceptKeyword("CROSS")) {
                    join = JoinType::Cross;
                }
                if (!acceptKeyword("JOIN")) {
                    if (at_ != mark)
                        fail();
                    break;
                }
            }
            FromItem item = fromItem();
            item.join = join;
            item.natural = natural;
            if (cur().isKeyword("ON") && !peek().isKeyword("CONFLICT")) {
                ++at_;
                item.on = expr();
            } else if (acceptKeyword("USING")) {
                expectPunct("(");
                do
                    item.usingColumns.push_back(name());
                while (acceptPunct(","));
                expectPunct(")");
            }
            if (natural && (item.on || !item.usingColumns.empty()))
                failWith("a NATURAL join may not have an ON or USING clause");
            items.push_back(std::move(item));
        }
        return items;
    }

    FromItem fromItem()
    {
        FromItem item;
        if (acceptPunct("(")) {
            enter();
            if (cur().isKeyword("SELECT") || cur().isKeyword("VALUES") || cur().isKeyword("WITH")) {
                item.kind = FromItem::Kind::Subquery;
                item.subquery = std::make_unique<SelectStmt>(selectStatement());
            } else {
                item.kind = FromItem::Kind::Group;
                item.group = fromClause();
            }
            leave();
            expectPunct(")");
        } else {
            qualifiedName(item.schema, item.name);
            if (acceptPunct("(")) {
                item.kind = FromItem::Kind::Function;
                if (!cur().isPunct(")")) {
                    do
                        item.args.push_back(expr());
                    while (acceptPunct(","));
                }
                expectPunct(")");
            }
        }
        if (acceptKeyword("AS"))
            item.alias = name();
        else if (atAlias())
            item.alias = alias();
        if (cur().isKeyword("INDEXED")) {
            ++at_;
            expectKeyword("BY");
            item.indexedBy = name();
        } else if (cur().isKeyword("NOT") && peek().isKeyword("INDEXED")) {
            at_ += 2;
            item.notIndexed = true;
        }
        return item;
    }

    WindowSpec windowDefinition()
    {
        WindowSpec w;
        expectPunct("(");
        if (cur().kind == Tok::Identifier && !cur().isKeyword("PARTITION") &&
            !cur().isKeyword("ORDER") && !cur().isKeyword("RANGE") && !cur().isKeyword("ROWS") &&
            !cur().isKeyword("GROUPS"))
            w.base = name();
        if (cur().isKeyword("PARTITION")) {
            ++at_;
            expectKeyword("BY");
            do
                w.partition.push_back(expr());
            while (acceptPunct(","));
        }
        if (cur().isKeyword("ORDER")) {
            ++at_;
            expectKeyword("BY");
            w.order = orderTerms();
        }
        if (cur().isKeyword("RANGE") || cur().isKeyword("ROWS") || cur().isKeyword("GROUPS")) {
            w.hasFrame = true;
            w.unit = acceptKeyword("RANGE") ? FrameUnit::Range
                     : acceptKeyword("ROWS") ? FrameUnit::Rows
                                             : (++at_, FrameUnit::Groups);
            if (acceptKeyword("BETWEEN")) {
                frameBound(w.startKind, w.startExpr);
                expectKeyword("AND");
                frameBound(w.endKind, w.endExpr);
            } else {
                frameBound(w.startKind, w.startExpr);
                w.endKind = FrameBound::CurrentRow;
            }
            if (acceptKeyword("EXCLUDE")) {
                if (acceptKeyword("NO")) {
                    expectKeyword("OTHERS");
                    w.exclude = FrameExclude::NoOthers;
                } else if (acceptKeyword("CURRENT")) {
                    expectKeyword("ROW");
                    w.exclude = FrameExclude::CurrentRow;
                } else if (acceptKeyword("GROUP")) {
                    w.exclude = FrameExclude::Group;
                } else {
                    expectKeyword("TIES");
                    w.exclude = FrameExclude::Ties;
                }
            }
        }
        expectPunct(")");
        return w;
    }

    void frameBound(FrameBound& kind, ExprPtr& value)
    {
        if (cur().isKeyword("UNBOUNDED")) {
            ++at_;
            if (acceptKeyword("PRECEDING"))
                kind = FrameBound::UnboundedPreceding;
            else {
                expectKeyword("FOLLOWING");
                kind = FrameBound::UnboundedFollowing;
            }
            return;
        }
        if (cur().isKeyword("CURRENT") && peek().isKeyword("ROW")) {
            at_ += 2;
            kind = FrameBound::CurrentRow;
            return;
        }
        value = expr();
        if (acceptKeyword("PRECEDING"))
            kind = FrameBound::Preceding;
        else {
            expectKeyword("FOLLOWING");
            kind = FrameBound::Following;
        }
    }
