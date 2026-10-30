// Included inside class Parser (parser.cpp): CREATE, DROP, ALTER.

    StatementBody create()
    {
        expectKeyword("CREATE");
        bool temp = false;
        if (acceptKeyword("TEMP") || acceptKeyword("TEMPORARY"))
            temp = true;
        if (acceptKeyword("TABLE"))
            return createTable(temp);
        if (acceptKeyword("VIEW"))
            return createView(temp);
        if (acceptKeyword("TRIGGER"))
            return createTrigger(temp);
        if (temp)
            fail();
        if (cur().isKeyword("VIRTUAL")) {
            ++at_;
            expectKeyword("TABLE");
            acceptIfNotExists();
            std::string schema, nm;
            qualifiedName(schema, nm);
            expectKeyword("USING");
            const std::string module = name();
            throw Error(ErrorCode::Unsupported, "no such module: " + module);
        }
        bool unique = acceptKeyword("UNIQUE");
        expectKeyword("INDEX");
        CreateIndexStmt s;
        s.unique = unique;
        s.ifNotExists = acceptIfNotExists();
        qualifiedName(s.schema, s.name, &s.nameBegin);
        expectKeyword("ON");
        s.table = name();
        s.columns = indexedColumns();
        if (acceptKeyword("WHERE"))
            s.where = expr();
        return s;
    }

    void foreignKeyClause(ForeignKeyDef& fk)
    {
        expectKeyword("REFERENCES");
        fk.table = name();
        if (acceptPunct("(")) {
            do
                fk.parentColumns.push_back(name());
            while (acceptPunct(","));
            expectPunct(")");
        }
        for (;;) {
            if (cur().isKeyword("ON") && (peek().isKeyword("DELETE") || peek().isKeyword("UPDATE"))) {
                ++at_;
                const bool del = acceptKeyword("DELETE");
                if (!del)
                    expectKeyword("UPDATE");
                FkAction a;
                if (acceptKeyword("SET")) {
                    if (acceptKeyword("NULL"))
                        a = FkAction::SetNull;
                    else {
                        expectKeyword("DEFAULT");
                        a = FkAction::SetDefault;
                    }
                } else if (acceptKeyword("CASCADE")) {
                    a = FkAction::Cascade;
                } else if (acceptKeyword("RESTRICT")) {
                    a = FkAction::Restrict;
                } else {
                    expectKeyword("NO");
                    expectKeyword("ACTION");
                    a = FkAction::NoAction;
                }
                (del ? fk.onDelete : fk.onUpdate) = a;
                continue;
            }
            if (acceptKeyword("MATCH")) {
                (void)name();
                continue;
            }
            break;
        }
        const std::size_t mark = at_;
        bool notDeferrable = false;
        if (acceptKeyword("NOT")) {
            if (!cur().isKeyword("DEFERRABLE")) {
                at_ = mark;
                return;
            }
            notDeferrable = true;
        }
        if (acceptKeyword("DEFERRABLE")) {
            bool deferred = false;
            if (acceptKeyword("INITIALLY")) {
                if (acceptKeyword("DEFERRED"))
                    deferred = true;
                else
                    expectKeyword("IMMEDIATE");
            }
            fk.deferred = deferred && !notDeferrable;
        }
    }

    Conflict onConflict()
    {
        if (cur().isKeyword("ON") && peek().isKeyword("CONFLICT")) {
            at_ += 2;
            return conflictAction();
        }
        return Conflict::Default;
    }

    ExprPtr defaultValue()
    {
        const std::uint32_t begin = cur().offset;
        if (cur().isPunct("(")) {
            ++at_;
            ExprPtr e = expr();
            expectPunct(")");
            e->begin = begin;
            e->end = prevEnd();
            return e;
        }
        if (cur().isPunct("-") || cur().isPunct("+")) {
            const bool neg = cur().isPunct("-");
            ++at_;
            if (cur().kind != Tok::Number)
                fail();
            if (neg && cur().minInt) {
                ++at_;
                return literal(Value(std::numeric_limits<std::int64_t>::min()), begin);
            }
            Value v = take().value;
            if (neg)
                v = v.type() == Type::Integer ? Value(-v.integer()) : Value(-v.real());
            return literal(std::move(v), begin);
        }
        const Token& t = cur();
        if (t.kind == Tok::Number || t.kind == Tok::String || t.kind == Tok::BlobLiteral) {
            ++at_;
            return literal(t.value, begin);
        }
        if (t.kind == Tok::Identifier) {
            if (!t.quoted) {
                if (t.isKeyword("NULL")) {
                    ++at_;
                    return literal(Value(), begin);
                }
                if (t.isKeyword("TRUE") || t.isKeyword("FALSE")) {
                    ++at_;
                    return literal(Value(std::int64_t(t.isKeyword("TRUE"))), begin);
                }
                if (t.isKeyword("CURRENT_TIME") || t.isKeyword("CURRENT_DATE") ||
                    t.isKeyword("CURRENT_TIMESTAMP"))
                    return primary();
            }
            // A bare identifier default is taken as a string, as SQLite does.
            ++at_;
            return literal(Value(t.text), begin);
        }
        fail();
    }

    ColumnDef columnDef()
    {
        ColumnDef c;
        c.name = name();
        c.type = typeName();
        for (;;) {
            std::string cname;
            if (acceptKeyword("CONSTRAINT"))
                cname = name();
            if (cur().isKeyword("PRIMARY")) {
                ++at_;
                expectKeyword("KEY");
                c.primaryKey = true;
                if (acceptKeyword("DESC"))
                    c.pkDesc = true;
                else
                    acceptKeyword("ASC");
                c.pkConflict = onConflict();
                if (acceptKeyword("AUTOINCREMENT"))
                    c.autoincrement = true;
            } else if (cur().isKeyword("NOT") && peek().isKeyword("NULL")) {
                at_ += 2;
                c.notNull = true;
                c.notNullConflict = onConflict();
            } else if (cur().isKeyword("NULL")) {
                ++at_;
                (void)onConflict();
            } else if (acceptKeyword("UNIQUE")) {
                c.unique = true;
                c.uniqueConflict = onConflict();
            } else if (acceptKeyword("CHECK")) {
                expectPunct("(");
                CheckDef ck;
                ck.name = cname;
                ck.expr = expr();
                expectPunct(")");
                c.checks.push_back(std::move(ck));
            } else if (acceptKeyword("DEFAULT")) {
                c.defaultExpr = defaultValue();
            } else if (acceptKeyword("COLLATE")) {
                c.collation = name();
            } else if (cur().isKeyword("REFERENCES")) {
                ForeignKeyDef fk;
                fk.columns.push_back(c.name);
                foreignKeyClause(fk);
                c.references.push_back(std::move(fk));
            } else if (cur().isKeyword("GENERATED") || cur().isKeyword("AS")) {
                if (acceptKeyword("GENERATED"))
                    expectKeyword("ALWAYS");
                expectKeyword("AS");
                expectPunct("(");
                c.generated = expr();
                expectPunct(")");
                if (acceptKeyword("STORED"))
                    c.stored = true;
                else
                    acceptKeyword("VIRTUAL");
            } else {
                if (!cname.empty())
                    fail();
                break;
            }
        }
        return c;
    }

    CreateTableStmt createTable(bool temp)
    {
        CreateTableStmt s;
        s.temp = temp;
        s.ifNotExists = acceptIfNotExists();
        qualifiedName(s.schema, s.name, &s.nameBegin);
        if (acceptKeyword("AS")) {
            s.asSelect = std::make_unique<SelectStmt>(selectStatement());
            return s;
        }
        expectPunct("(");
        bool constraints = false;
        for (;;) {
            if (!constraints && (cur().isKeyword("CONSTRAINT") || cur().isKeyword("PRIMARY") ||
                                 cur().isKeyword("UNIQUE") || cur().isKeyword("CHECK") ||
                                 cur().isKeyword("FOREIGN")))
                constraints = true;
            if (!constraints) {
                s.columns.push_back(columnDef());
            } else {
                TableConstraint tc;
                if (acceptKeyword("CONSTRAINT"))
                    tc.name = name();
                if (cur().isKeyword("PRIMARY")) {
                    ++at_;
                    expectKeyword("KEY");
                    tc.kind = TableConstraint::Kind::PrimaryKey;
                    tc.columns = indexedColumns();
                    if (acceptKeyword("AUTOINCREMENT"))
                        tc.autoincrement = true;
                    tc.conflict = onConflict();
                } else if (acceptKeyword("UNIQUE")) {
                    tc.kind = TableConstraint::Kind::Unique;
                    tc.columns = indexedColumns();
                    tc.conflict = onConflict();
                } else if (acceptKeyword("CHECK")) {
                    tc.kind = TableConstraint::Kind::Check;
                    expectPunct("(");
                    tc.check = expr();
                    expectPunct(")");
                    (void)onConflict();
                } else if (acceptKeyword("FOREIGN")) {
                    expectKeyword("KEY");
                    tc.kind = TableConstraint::Kind::ForeignKey;
                    expectPunct("(");
                    do
                        tc.fk.columns.push_back(name());
                    while (acceptPunct(","));
                    expectPunct(")");
                    foreignKeyClause(tc.fk);
                } else {
                    fail();
                }
                s.constraints.push_back(std::move(tc));
            }
            if (acceptPunct(","))
                continue;
            if (acceptPunct(")"))
                break;
            // SQLite tolerates table constraints without separating commas.
            if (constraints && (cur().isKeyword("CONSTRAINT") || cur().isKeyword("PRIMARY") ||
                                cur().isKeyword("UNIQUE") || cur().isKeyword("CHECK") ||
                                cur().isKeyword("FOREIGN")))
                continue;
            fail();
        }
        for (;;) {
            if (acceptKeyword("WITHOUT")) {
                if (!cur().isKeyword("ROWID"))
                    failWith("unknown table option: " + cur().text);
                ++at_;
                s.withoutRowid = true;
            } else if (acceptKeyword("STRICT")) {
                s.strict = true;
            } else {
                break;
            }
            if (!acceptPunct(","))
                break;
        }
        return s;
    }

    CreateViewStmt createView(bool temp)
    {
        CreateViewStmt s;
        s.temp = temp;
        s.ifNotExists = acceptIfNotExists();
        qualifiedName(s.schema, s.name, &s.nameBegin);
        if (acceptPunct("(")) {
            do
                s.columns.push_back(name());
            while (acceptPunct(","));
            expectPunct(")");
        }
        expectKeyword("AS");
        s.select = std::make_unique<SelectStmt>(selectStatement());
        return s;
    }

    CreateTriggerStmt createTrigger(bool temp)
    {
        CreateTriggerStmt s;
        s.temp = temp;
        s.ifNotExists = acceptIfNotExists();
        qualifiedName(s.schema, s.name, &s.nameBegin);
        if (acceptKeyword("BEFORE")) {
            s.time = TriggerTime::Before;
        } else if (acceptKeyword("AFTER")) {
            s.time = TriggerTime::After;
        } else if (acceptKeyword("INSTEAD")) {
            expectKeyword("OF");
            s.time = TriggerTime::InsteadOf;
        }
        if (acceptKeyword("DELETE")) {
            s.event = TriggerEvent::Delete;
        } else if (acceptKeyword("INSERT")) {
            s.event = TriggerEvent::Insert;
        } else {
            expectKeyword("UPDATE");
            s.event = TriggerEvent::Update;
            if (acceptKeyword("OF")) {
                do
                    s.updateOf.push_back(name());
                while (acceptPunct(","));
            }
        }
        expectKeyword("ON");
        std::string schema;
        qualifiedName(schema, s.table);
        if (acceptKeyword("FOR")) {
            expectKeyword("EACH");
            expectKeyword("ROW");
        }
        if (acceptKeyword("WHEN"))
            s.when = expr();
        expectKeyword("BEGIN");
        do {
            auto node = std::make_unique<StatementNode>();
            node->begin = cur().offset;
            if (cur().isKeyword("WITH")) {
                auto with = withClause();
                if (cur().isKeyword("INSERT") || cur().isKeyword("REPLACE"))
                    node->body = insert(std::move(with));
                else if (cur().isKeyword("UPDATE"))
                    node->body = update(std::move(with));
                else if (cur().isKeyword("DELETE"))
                    node->body = remove(std::move(with));
                else
                    node->body = selectAfterWith(std::move(with));
            } else if (cur().isKeyword("INSERT") || cur().isKeyword("REPLACE")) {
                node->body = insert(nullptr);
            } else if (cur().isKeyword("UPDATE")) {
                node->body = update(nullptr);
            } else if (cur().isKeyword("DELETE")) {
                node->body = remove(nullptr);
            } else if (cur().isKeyword("SELECT") || cur().isKeyword("VALUES")) {
                node->body = selectStatement();
            } else {
                fail();
            }
            node->end = prevEnd();
            s.body.push_back(std::move(node));
            expectPunct(";");
        } while (!cur().isKeyword("END"));
        expectKeyword("END");
        return s;
    }

    DropStmt drop()
    {
        expectKeyword("DROP");
        DropStmt s;
        if (acceptKeyword("TABLE"))
            s.kind = DropStmt::Kind::Table;
        else if (acceptKeyword("INDEX"))
            s.kind = DropStmt::Kind::Index;
        else if (acceptKeyword("VIEW"))
            s.kind = DropStmt::Kind::View;
        else {
            expectKeyword("TRIGGER");
            s.kind = DropStmt::Kind::Trigger;
        }
        s.ifExists = acceptIfExists();
        qualifiedName(s.schema, s.name);
        return s;
    }

    AlterTableStmt alter()
    {
        expectKeyword("ALTER");
        expectKeyword("TABLE");
        AlterTableStmt s;
        qualifiedName(s.schema, s.table);
        if (acceptKeyword("RENAME")) {
            if (acceptKeyword("TO")) {
                s.action = AlterTableStmt::Action::RenameTable;
                s.newName = name();
                return s;
            }
            acceptKeyword("COLUMN");
            s.action = AlterTableStmt::Action::RenameColumn;
            s.column = name();
            expectKeyword("TO");
            s.newName = name();
            return s;
        }
        if (acceptKeyword("ADD")) {
            acceptKeyword("COLUMN");
            s.action = AlterTableStmt::Action::AddColumn;
            s.defBegin = cur().offset;
            s.def = columnDef();
            s.defEnd = prevEnd();
            return s;
        }
        expectKeyword("DROP");
        acceptKeyword("COLUMN");
        s.action = AlterTableStmt::Action::DropColumn;
        s.column = name();
        return s;
    }
