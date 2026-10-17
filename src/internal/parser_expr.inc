// Included inside class Parser (parser.cpp): expressions, with SQLite's
// operator precedence, loosest first:
//
//   OR
//   AND
//   NOT
//   = == <> != IS [NOT] IN LIKE GLOB MATCH REGEXP BETWEEN ISNULL NOTNULL
//   < <= > >=
//   & | << >>
//   + -
//   * / %
//   || -> ->>
//   COLLATE
//   unary - + ~

    static void settle(Expr& e)
    {
        unsigned h = 0;
        if (e.lhs)
            h = std::max(h, e.lhs->height);
        if (e.rhs)
            h = std::max(h, e.rhs->height);
        if (e.extra)
            h = std::max(h, e.extra->height);
        for (const ExprPtr& x : e.list)
            h = std::max(h, x->height);
        e.height = h + 1;
        if (e.height > kMaxExprHeight)
            throw Error(ErrorCode::SyntaxError, "Expression tree is too large (maximum depth " +
                                                    std::to_string(kMaxExprHeight) + ")");
    }

    ExprPtr binary(BinaryOp op, ExprPtr l, ExprPtr r)
    {
        ExprPtr e = makeExpr(ExprKind::Binary);
        e->op = std::uint8_t(op);
        e->begin = l->begin;
        e->end = r->end;
        e->lhs = std::move(l);
        e->rhs = std::move(r);
        settle(*e);
        return e;
    }

    ExprPtr expr()
    {
        enter();
        ExprPtr e = orExpr();
        leave();
        return e;
    }

    ExprPtr orExpr()
    {
        ExprPtr e = andExpr();
        while ((cur().ops & kOpOr) && acceptKeyword("OR"))
            e = binary(BinaryOp::Or, std::move(e), andExpr());
        return e;
    }

    ExprPtr andExpr()
    {
        ExprPtr e = notExpr();
        while ((cur().ops & kOpAnd) && acceptKeyword("AND"))
            e = binary(BinaryOp::And, std::move(e), notExpr());
        return e;
    }

    ExprPtr notExpr()
    {
        if (cur().isKeyword("NOT")) {
            const std::uint32_t begin = cur().offset;
            ++at_;
            enter();
            ExprPtr inner = notExpr();
            leave();
            ExprPtr e = makeExpr(ExprKind::Unary);
            e->op = std::uint8_t(UnaryOp::Not);
            e->begin = begin;
            e->end = inner->end;
            e->lhs = std::move(inner);
            settle(*e);
            return e;
        }
        return equality();
    }

    ExprPtr equality()
    {
        ExprPtr e = relational();
        for (;;) {
            if (!(cur().ops & kOpEquality))
                break;
            const std::uint32_t begin = e->begin;
            if (acceptPunct("=") || acceptPunct("==")) {
                e = binary(BinaryOp::Eq, std::move(e), relational());
                continue;
            }
            if (acceptPunct("<>") || acceptPunct("!=")) {
                e = binary(BinaryOp::Ne, std::move(e), relational());
                continue;
            }
            if (acceptKeyword("IS")) {
                bool negated = acceptKeyword("NOT");
                if (acceptKeyword("DISTINCT")) {
                    expectKeyword("FROM");
                    negated = !negated;
                }
                ExprPtr r = relational();
                ExprPtr x = makeExpr(ExprKind::Is);
                x->negated = negated;
                x->begin = begin;
                x->end = r->end;
                x->lhs = std::move(e);
                x->rhs = std::move(r);
                settle(*x);
                e = std::move(x);
                continue;
            }
            if (cur().isKeyword("ISNULL") || cur().isKeyword("NOTNULL") ||
                (cur().isKeyword("NOT") && peek().isKeyword("NULL"))) {
                const bool negated = !cur().isKeyword("ISNULL");
                at_ += cur().isKeyword("NOT") ? 2 : 1;
                ExprPtr x = makeExpr(ExprKind::Is);
                x->negated = negated;
                x->begin = begin;
                x->end = prevEnd();
                x->lhs = std::move(e);
                x->rhs = makeExpr(ExprKind::Literal);
                settle(*x);
                e = std::move(x);
                continue;
            }
            const std::size_t mark = at_;
            const bool negated = acceptKeyword("NOT");
            if (acceptKeyword("IN")) {
                e = inExpr(std::move(e), negated);
                continue;
            }
            if (acceptKeyword("BETWEEN")) {
                ExprPtr x = makeExpr(ExprKind::Between);
                x->negated = negated;
                x->begin = begin;
                x->lhs = std::move(e);
                x->list.push_back(relational());
                expectKeyword("AND");
                x->list.push_back(relational());
                x->end = prevEnd();
                settle(*x);
                e = std::move(x);
                continue;
            }
            LikeKind kind;
            if (cur().isKeyword("LIKE"))
                kind = LikeKind::Like;
            else if (cur().isKeyword("GLOB"))
                kind = LikeKind::Glob;
            else if (cur().isKeyword("REGEXP"))
                kind = LikeKind::Regexp;
            else if (cur().isKeyword("MATCH"))
                kind = LikeKind::Match;
            else {
                at_ = mark;
                break;
            }
            ++at_;
            ExprPtr x = makeExpr(ExprKind::Like);
            x->op = std::uint8_t(kind);
            x->negated = negated;
            x->begin = begin;
            x->lhs = std::move(e);
            x->rhs = relational();
            if (acceptKeyword("ESCAPE"))
                x->extra = bitwise();
            x->end = prevEnd();
            settle(*x);
            e = std::move(x);
        }
        return e;
    }

    ExprPtr inExpr(ExprPtr lhs, bool negated)
    {
        const std::uint32_t begin = lhs->begin;
        if (acceptPunct("(")) {
            if (cur().isKeyword("SELECT") || cur().isKeyword("VALUES") || cur().isKeyword("WITH")) {
                ExprPtr x = makeExpr(ExprKind::InSelect);
                x->negated = negated;
                x->begin = begin;
                x->lhs = std::move(lhs);
                enter();
                x->select = std::make_unique<SelectStmt>(selectStatement());
                leave();
                expectPunct(")");
                x->end = prevEnd();
                settle(*x);
                return x;
            }
            ExprPtr x = makeExpr(ExprKind::InList);
            x->negated = negated;
            x->begin = begin;
            x->lhs = std::move(lhs);
            if (!cur().isPunct(")")) {
                do
                    x->list.push_back(expr());
                while (acceptPunct(","));
            }
            expectPunct(")");
            x->end = prevEnd();
            settle(*x);
            return x;
        }
        // `x IN table` or `x IN table_function(args)`: sugar for a select.
        ExprPtr x = makeExpr(ExprKind::InSelect);
        x->negated = negated;
        x->begin = begin;
        x->lhs = std::move(lhs);
        auto select = std::make_unique<SelectStmt>();
        SelectCore core;
        ResultColumn star;
        star.star = true;
        core.columns.push_back(std::move(star));
        FromItem item;
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
        core.from.push_back(std::move(item));
        select->cores.push_back(std::move(core));
        x->select = std::move(select);
        x->end = prevEnd();
        settle(*x);
        return x;
    }

    ExprPtr relational()
    {
        ExprPtr e = bitwise();
        for (;;) {
            if (!(cur().ops & kOpRelational))
                return e;
            BinaryOp op;
            if (acceptPunct("<"))
                op = BinaryOp::Lt;
            else if (acceptPunct("<="))
                op = BinaryOp::Le;
            else if (acceptPunct(">"))
                op = BinaryOp::Gt;
            else if (acceptPunct(">="))
                op = BinaryOp::Ge;
            else
                return e;
            e = binary(op, std::move(e), bitwise());
        }
    }

    ExprPtr bitwise()
    {
        ExprPtr e = additive();
        for (;;) {
            if (!(cur().ops & kOpBitwise))
                return e;
            BinaryOp op;
            if (acceptPunct("&"))
                op = BinaryOp::BitAnd;
            else if (acceptPunct("|"))
                op = BinaryOp::BitOr;
            else if (acceptPunct("<<"))
                op = BinaryOp::ShiftLeft;
            else if (acceptPunct(">>"))
                op = BinaryOp::ShiftRight;
            else
                return e;
            e = binary(op, std::move(e), additive());
        }
    }

    ExprPtr additive()
    {
        ExprPtr e = multiplicative();
        for (;;) {
            if (!(cur().ops & kOpAdditive))
                return e;
            BinaryOp op;
            if (acceptPunct("+"))
                op = BinaryOp::Add;
            else if (acceptPunct("-"))
                op = BinaryOp::Sub;
            else
                return e;
            e = binary(op, std::move(e), multiplicative());
        }
    }

    ExprPtr multiplicative()
    {
        ExprPtr e = concat();
        for (;;) {
            if (!(cur().ops & kOpMultiplicative))
                return e;
            BinaryOp op;
            if (acceptPunct("*"))
                op = BinaryOp::Mul;
            else if (acceptPunct("/"))
                op = BinaryOp::Div;
            else if (acceptPunct("%"))
                op = BinaryOp::Mod;
            else
                return e;
            e = binary(op, std::move(e), concat());
        }
    }

    ExprPtr concat()
    {
        ExprPtr e = collate();
        for (;;) {
            if (!(cur().ops & kOpConcat))
                return e;
            if (acceptPunct("||")) {
                e = binary(BinaryOp::Concat, std::move(e), collate());
                continue;
            }
            if (cur().isPunct("->") || cur().isPunct("->>")) {
                const std::string fn = take().text;
                ExprPtr r = collate();
                ExprPtr x = makeExpr(ExprKind::Function);
                x->name = fn;
                x->begin = e->begin;
                x->end = r->end;
                x->list.push_back(std::move(e));
                x->list.push_back(std::move(r));
                settle(*x);
                e = std::move(x);
                continue;
            }
            return e;
        }
    }

    ExprPtr collate()
    {
        ExprPtr e = unary();
        while ((cur().ops & kOpCollate) && acceptKeyword("COLLATE")) {
            ExprPtr x = makeExpr(ExprKind::Collate);
            x->begin = e->begin;
            x->name = name();
            x->end = prevEnd();
            x->lhs = std::move(e);
            settle(*x);
            e = std::move(x);
        }
        return e;
    }

    ExprPtr unary()
    {
        const Token& t = cur();
        UnaryOp op;
        if (t.isPunct("-"))
            op = UnaryOp::Negate;
        else if (t.isPunct("+"))
            op = UnaryOp::Plus;
        else if (t.isPunct("~"))
            op = UnaryOp::BitNot;
        else
            return primary();
        const std::uint32_t begin = t.offset;
        ++at_;
        // -9223372036854775808 is the one integer literal that only fits
        // once negated.
        if (op == UnaryOp::Negate && cur().kind == Tok::Number && cur().minInt) {
            ExprPtr x = makeExpr(ExprKind::Literal);
            x->literal = Value(std::numeric_limits<std::int64_t>::min());
            x->begin = begin;
            ++at_;
            x->end = prevEnd();
            return x;
        }
        enter();
        ExprPtr inner = unary();
        leave();
        ExprPtr x = makeExpr(ExprKind::Unary);
        x->op = std::uint8_t(op);
        x->begin = begin;
        x->end = inner->end;
        x->lhs = std::move(inner);
        settle(*x);
        return x;
    }

    ExprPtr literal(Value v, std::uint32_t begin)
    {
        ExprPtr e = makeExpr(ExprKind::Literal);
        e->literal = std::move(v);
        e->begin = begin;
        e->end = prevEnd();
        return e;
    }

    std::size_t parameterIndex(const std::string& spelling)
    {
        std::size_t index = 0;
        if (spelling == "?") {
            index = ++maxParam_;
        } else if (spelling[0] == '?') {
            const std::string digits = spelling.substr(1);
            if (digits.size() > 6 || std::stoul(digits) < 1 || std::stoul(digits) > kMaxVariable)
                failWith("variable number must be between ?1 and ?" + std::to_string(kMaxVariable));
            index = std::stoul(digits);
            maxParam_ = std::max(maxParam_, index);
        } else {
            const auto it = namedParams_.find(spelling);
            if (it != namedParams_.end())
                return it->second - 1;
            index = ++maxParam_;
            namedParams_[spelling] = index;
        }
        if (maxParam_ > kMaxVariable)
            failWith("too many SQL variables");
        if (paramNames_.size() < maxParam_)
            paramNames_.resize(maxParam_);
        if (spelling != "?")
            paramNames_[index - 1] = spelling;
        return index - 1;
    }

    ExprPtr primary()
    {
        const Token& t = cur();
        const std::uint32_t begin = t.offset;
        switch (t.kind) {
            case Tok::Number:
            case Tok::String:
            case Tok::BlobLiteral: ++at_; return literal(t.value, begin);
            case Tok::Parameter: {
                ++at_;
                ExprPtr e = makeExpr(ExprKind::Parameter);
                e->param = parameterIndex(t.text);
                e->name = t.text;
                e->begin = begin;
                e->end = prevEnd();
                return e;
            }
            case Tok::Punct:
                if (t.isPunct("("))
                    return parenthesized();
                fail();
            case Tok::End: fail();
            case Tok::Identifier: break;
        }
        if (!t.quoted) {
            if (t.isKeyword("NULL")) {
                ++at_;
                return literal(Value(), begin);
            }
            if ((t.isKeyword("TRUE") || t.isKeyword("FALSE")) && !peek().isPunct(".") &&
                !peek().isPunct("(")) {
                const bool v = t.isKeyword("TRUE");
                ++at_;
                return literal(Value(std::int64_t(v)), begin);
            }
            if (t.isKeyword("CURRENT_TIME") || t.isKeyword("CURRENT_DATE") ||
                t.isKeyword("CURRENT_TIMESTAMP")) {
                ExprPtr e = makeExpr(ExprKind::Function);
                e->name = toLower(take().text);
                e->begin = begin;
                e->end = prevEnd();
                return e;
            }
            if (t.isKeyword("CAST") && peek().isPunct("("))
                return castExpr();
            if (t.isKeyword("CASE"))
                return caseExpr();
            if (t.isKeyword("EXISTS") && peek().isPunct("(")) {
                at_ += 2;
                ExprPtr e = makeExpr(ExprKind::Exists);
                e->begin = begin;
                enter();
                e->select = std::make_unique<SelectStmt>(selectStatement());
                leave();
                expectPunct(")");
                e->end = prevEnd();
                return e;
            }
            if (t.isKeyword("RAISE") && peek().isPunct("("))
                return raiseExpr();
            if (t.reserved)
                fail();
        }
        ++at_;
        if (cur().isPunct("(") && !t.quoted)
            return functionCall(t.text, begin);
        ExprPtr e = makeExpr(ExprKind::Column);
        e->name = t.text;
        if (acceptPunct(".")) {
            e->table = std::move(e->name);
            e->name = name();
            if (acceptPunct(".")) {
                e->schema = std::move(e->table);
                e->table = std::move(e->name);
                e->name = name();
            }
        }
        e->begin = begin;
        e->end = prevEnd();
        return e;
    }

    ExprPtr parenthesized()
    {
        const std::uint32_t begin = cur().offset;
        expectPunct("(");
        if (cur().isKeyword("SELECT") || cur().isKeyword("VALUES") || cur().isKeyword("WITH")) {
            ExprPtr e = makeExpr(ExprKind::Subquery);
            e->select = std::make_unique<SelectStmt>(selectStatement());
            expectPunct(")");
            e->begin = begin;
            e->end = prevEnd();
            return e;
        }
        ExprPtr first = expr();
        if (acceptPunct(",")) {
            ExprPtr row = makeExpr(ExprKind::RowValue);
            row->list.push_back(std::move(first));
            do
                row->list.push_back(expr());
            while (acceptPunct(","));
            expectPunct(")");
            row->begin = begin;
            row->end = prevEnd();
            settle(*row);
            return row;
        }
        expectPunct(")");
        // Keep the parentheses in the span, so a result column is named
        // "(a+b)" as written.
        first->begin = begin;
        first->end = prevEnd();
        return first;
    }

    std::string typeName()
    {
        // A type name is one or more words, optionally followed by one or two
        // signed numbers in parentheses. Words stop at anything that starts a
        // column constraint.
        std::string out;
        while (cur().kind == Tok::Identifier || cur().kind == Tok::String) {
            const Token& t = cur();
            if (!t.quoted &&
                (t.isKeyword("CONSTRAINT") || t.isKeyword("PRIMARY") || t.isKeyword("NOT") ||
                 t.isKeyword("NULL") || t.isKeyword("UNIQUE") || t.isKeyword("CHECK") ||
                 t.isKeyword("DEFAULT") || t.isKeyword("COLLATE") || t.isKeyword("REFERENCES") ||
                 t.isKeyword("GENERATED") || t.isKeyword("AS")))
                break;
            if (!out.empty())
                out += ' ';
            out += t.kind == Tok::String ? t.value.text() : t.text;
            ++at_;
        }
        if (!out.empty() && cur().isPunct("(")) {
            const std::uint32_t from = cur().offset;
            int depth = 0;
            do {
                if (cur().isPunct("("))
                    ++depth;
                else if (cur().isPunct(")"))
                    --depth;
                else if (cur().kind == Tok::End)
                    fail();
                ++at_;
            } while (depth > 0);
            out += std::string(text_.substr(from, prevEnd() - from));
        }
        return out;
    }

    ExprPtr castExpr()
    {
        const std::uint32_t begin = cur().offset;
        at_ += 2;  // CAST (
        ExprPtr e = makeExpr(ExprKind::Cast);
        e->lhs = expr();
        expectKeyword("AS");
        e->name = typeName();
        expectPunct(")");
        e->begin = begin;
        e->end = prevEnd();
        settle(*e);
        return e;
    }

    ExprPtr caseExpr()
    {
        const std::uint32_t begin = cur().offset;
        ++at_;  // CASE
        ExprPtr e = makeExpr(ExprKind::Case);
        if (!cur().isKeyword("WHEN"))
            e->lhs = expr();
        if (!cur().isKeyword("WHEN"))
            fail();
        while (acceptKeyword("WHEN")) {
            e->list.push_back(expr());
            expectKeyword("THEN");
            e->list.push_back(expr());
        }
        if (acceptKeyword("ELSE"))
            e->rhs = expr();
        expectKeyword("END");
        e->begin = begin;
        e->end = prevEnd();
        settle(*e);
        return e;
    }

    ExprPtr raiseExpr()
    {
        const std::uint32_t begin = cur().offset;
        at_ += 2;  // RAISE (
        ExprPtr e = makeExpr(ExprKind::Raise);
        if (acceptKeyword("IGNORE")) {
            e->op = std::uint8_t(RaiseKind::Ignore);
        } else {
            if (acceptKeyword("ROLLBACK"))
                e->op = std::uint8_t(RaiseKind::Rollback);
            else if (acceptKeyword("ABORT"))
                e->op = std::uint8_t(RaiseKind::Abort);
            else if (acceptKeyword("FAIL"))
                e->op = std::uint8_t(RaiseKind::Fail);
            else
                fail();
            expectPunct(",");
            if (cur().kind != Tok::String)
                fail();
            e->literal = take().value;
        }
        expectPunct(")");
        e->begin = begin;
        e->end = prevEnd();
        return e;
    }

    ExprPtr functionCall(const std::string& fname, std::uint32_t begin)
    {
        if (!knownFunction(fname))
            throw Error(ErrorCode::Unsupported, "no such function: " + fname);
        expectPunct("(");
        ExprPtr e = makeExpr(ExprKind::Function);
        e->name = fname;
        if (acceptPunct("*")) {
            e->star = true;
        } else if (!cur().isPunct(")")) {
            if (acceptKeyword("DISTINCT"))
                e->distinct = true;
            else
                acceptKeyword("ALL");
            do
                e->list.push_back(expr());
            while (acceptPunct(","));
            if (cur().isKeyword("ORDER") && peek().isKeyword("BY")) {
                at_ += 2;
                e->orderBy = orderTerms();
            }
        }
        expectPunct(")");
        if (cur().isKeyword("FILTER") && peek().isPunct("(")) {
            at_ += 2;
            expectKeyword("WHERE");
            e->filter = expr();
            expectPunct(")");
        }
        if (cur().isKeyword("OVER")) {
            ++at_;
            e->kind = ExprKind::Window;
            if (cur().isPunct("(")) {
                e->window = std::make_unique<WindowSpec>(windowDefinition());
            } else {
                e->window = std::make_unique<WindowSpec>();
                e->window->base = name();
                e->window->hasFrame = false;
                // A bare name: the whole definition comes from the WINDOW clause.
                e->window->partition.clear();
                e->window->unit = FrameUnit::Range;
                e->literal = Value(std::int64_t(1));  // marks "OVER name" (no parentheses)
            }
        }
        e->begin = begin;
        e->end = prevEnd();
        settle(*e);
        return e;
    }
