// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Recursive-descent parser. Precedence climbing for expressions, one method
// per statement shape.

#include "internal/parser.hpp"

#include <algorithm>
#include <optional>

#include "internal/lexer.hpp"

namespace sql::internal {

bool mutates(const Statement& s) noexcept
{
    return !std::holds_alternative<SelectStmt>(s);
}

const char* toString(AggregateOp op) noexcept
{
    switch (op) {
        case AggregateOp::Count: return "COUNT";
        case AggregateOp::Sum: return "SUM";
        case AggregateOp::Avg: return "AVG";
        case AggregateOp::Min: return "MIN";
        case AggregateOp::Max: return "MAX";
    }
    return "COUNT";
}

namespace {

const char* spell(BinaryOp op) noexcept
{
    switch (op) {
        case BinaryOp::Or: return " OR ";
        case BinaryOp::And: return " AND ";
        case BinaryOp::Eq: return " = ";
        case BinaryOp::Ne: return " <> ";
        case BinaryOp::Lt: return " < ";
        case BinaryOp::Le: return " <= ";
        case BinaryOp::Gt: return " > ";
        case BinaryOp::Ge: return " >= ";
        case BinaryOp::Add: return " + ";
        case BinaryOp::Sub: return " - ";
        case BinaryOp::Mul: return " * ";
        case BinaryOp::Div: return " / ";
        case BinaryOp::Mod: return " % ";
        case BinaryOp::Concat: return " || ";
    }
    return " ? ";
}

/// Words that can never be an unquoted table or result alias, so that
/// `SELECT a FROM t WHERE ...` does not read WHERE as an alias.
bool isReserved(const Token& t) noexcept
{
    static const char* kWords[] = {
        "SELECT", "FROM",  "WHERE", "INSERT", "INTO",  "VALUES", "UPDATE", "SET",
        "DELETE", "CREATE", "DROP",  "TABLE",  "INDEX", "ON",     "INNER",  "LEFT",
        "RIGHT",  "FULL",  "OUTER", "CROSS",  "JOIN",  "ORDER",  "GROUP",  "HAVING",
        "BY",     "LIMIT", "OFFSET", "AS",    "AND",   "OR",     "NOT",    "IS",
        "IN",     "BETWEEN", "LIKE", "ASC",   "DESC",  "NULL",   "PRIMARY", "KEY",
        "UNIQUE", "DEFAULT", "IF",  "EXISTS", "FOR",
    };
    if (t.kind != Tok::Identifier || t.quoted)
        return false;
    for (const char* w : kWords) {
        if (equalsNoCase(t.text, w))
            return true;
    }
    return false;
}

class Parser
{
public:
    explicit Parser(std::string_view text) : toks_(tokenize(checked(text)))
    {
        if (toks_.size() > kMaxTokens)
            throw Error(ErrorCode::SyntaxError, "statement has too many tokens");
    }

    /// Valid once `all()` has run.
    std::size_t parameterCount() const noexcept { return params_; }

    std::vector<Statement> all()
    {
        std::vector<Statement> out;
        for (;;) {
            while (acceptPunct(";")) {
            }
            if (cur().kind == Tok::End)
                break;
            out.push_back(statement());
            if (cur().kind != Tok::End && !cur().isPunct(";"))
                fail("expected ';' between statements");
        }
        if (out.empty())
            fail("no statement to run");
        return out;
    }

private:
    // --- token plumbing ---
    const Token& cur() const { return toks_[at_]; }
    const Token& peek(std::size_t n = 1) const
    {
        return toks_[std::min(at_ + n, toks_.size() - 1)];
    }
    Token take() { return toks_[at_ < toks_.size() - 1 ? at_++ : at_]; }

    [[noreturn]] void fail(const std::string& what) const
    {
        std::string saw = cur().kind == Tok::End ? "end of statement" : ("'" + cur().text + "'");
        if (cur().kind == Tok::Number || cur().kind == Tok::String ||
            cur().kind == Tok::BlobLiteral)
            saw = cur().value.toLiteral();
        throw Error(ErrorCode::SyntaxError, what + ", but found " + saw);
    }

    bool acceptKeyword(std::string_view w)
    {
        if (!cur().isKeyword(w))
            return false;
        ++at_;
        return true;
    }
    void expectKeyword(std::string_view w)
    {
        if (!acceptKeyword(w))
            fail("expected " + std::string(w));
    }
    bool acceptPunct(std::string_view p)
    {
        if (!cur().isPunct(p))
            return false;
        ++at_;
        return true;
    }
    void expectPunct(std::string_view p)
    {
        if (!acceptPunct(p))
            fail("expected '" + std::string(p) + "'");
    }
    std::string identifier()
    {
        if (cur().kind != Tok::Identifier)
            fail("expected a name");
        return take().text;
    }
    /// A name that must not collide with the clause keywords around it.
    std::string freshName()
    {
        if (isReserved(cur()))
            fail("expected a name");
        return identifier();
    }
    bool acceptIfNotExists()
    {
        if (!acceptKeyword("IF"))
            return false;
        expectKeyword("NOT");
        expectKeyword("EXISTS");
        return true;
    }
    bool acceptIfExists()
    {
        if (!acceptKeyword("IF"))
            return false;
        expectKeyword("EXISTS");
        return true;
    }

    static ExprPtr node(ExprKind k)
    {
        auto e = std::make_unique<Expr>();
        e->kind = k;
        return e;
    }

    // --- statements ---
    Statement statement()
    {
        if (acceptKeyword("CREATE")) {
            const bool unique = acceptKeyword("UNIQUE");
            if (acceptKeyword("TABLE")) {
                if (unique)
                    fail("UNIQUE is not a table modifier");
                return createTable();
            }
            expectKeyword("INDEX");
            return createIndex(unique);
        }
        if (acceptKeyword("DROP")) {
            if (acceptKeyword("TABLE")) {
                DropTableStmt s;
                s.ifExists = acceptIfExists();
                s.name = freshName();
                return s;
            }
            expectKeyword("INDEX");
            DropIndexStmt s;
            s.ifExists = acceptIfExists();
            s.name = freshName();
            return s;
        }
        if (acceptKeyword("INSERT"))
            return insert();
        if (acceptKeyword("SELECT"))
            return select();
        if (acceptKeyword("UPDATE"))
            return update();
        if (acceptKeyword("DELETE"))
            return remove();
        fail("expected a statement");
    }

    Statement createTable()
    {
        CreateTableStmt s;
        s.ifNotExists = acceptIfNotExists();
        s.name = freshName();
        expectPunct("(");
        do {
            if (cur().isKeyword("PRIMARY")) {
                ++at_;
                expectKeyword("KEY");
                if (!s.tablePrimaryKey.empty())
                    fail("a table may declare only one PRIMARY KEY");
                s.tablePrimaryKey = columnList();
                continue;
            }
            if (cur().isKeyword("UNIQUE") && peek().isPunct("(")) {
                ++at_;
                s.uniques.push_back(columnList());
                continue;
            }
            s.columns.push_back(columnDef());
        } while (acceptPunct(","));
        expectPunct(")");

        // Table-level constraints are checked against the declared columns
        // here; whether they make a sensible key is the executor's business.
        const auto declared = [&](const std::string& name, const char* what) {
            const bool known = std::any_of(s.columns.begin(), s.columns.end(),
                                           [&](const ColumnInfo& c) {
                                               return equalsNoCase(c.name, name);
                                           });
            if (!known)
                throw Error(ErrorCode::NoSuchColumn,
                            std::string(what) + " names an undeclared column '" + name + "'");
        };
        for (const std::string& name : s.tablePrimaryKey)
            declared(name, "PRIMARY KEY");
        for (const auto& unique : s.uniques) {
            for (const std::string& name : unique)
                declared(name, "UNIQUE");
        }
        return s;
    }

    /// `(a, b, ...)` with at least one name.
    std::vector<std::string> columnList()
    {
        std::vector<std::string> out;
        expectPunct("(");
        do {
            out.push_back(identifier());
        } while (acceptPunct(","));
        expectPunct(")");
        return out;
    }

    ColumnInfo columnDef()
    {
        ColumnInfo c;
        c.name = freshName();
        if (cur().kind != Tok::Identifier)
            fail("expected a type for column '" + c.name + "'");
        const std::string typeName = take().text;
        if (const auto t = typeFromName(typeName))
            c.type = *t;
        else
            throw Error(ErrorCode::Unsupported, "unknown column type '" + typeName + "'");
        if (acceptPunct("(")) {  // VARCHAR(255) and friends: the width is noise
            while (!cur().isPunct(")") && cur().kind != Tok::End)
                ++at_;
            expectPunct(")");
        }

        for (;;) {
            if (acceptKeyword("PRIMARY")) {
                expectKeyword("KEY");
                c.primaryKey = true;
            } else if (acceptKeyword("UNIQUE")) {
                c.unique = true;
            } else if (acceptKeyword("DEFAULT")) {
                c.hasDefault = true;
                c.defaultValue = constant();
            } else if (cur().isKeyword("NOT") && peek().isKeyword("NULL")) {
                at_ += 2;
                c.notNull = true;
            } else if (cur().isKeyword("NULL")) {
                ++at_;
            } else {
                break;
            }
        }
        return c;
    }

    /// A DEFAULT must be settled at CREATE TABLE time, so only signed literals.
    Value constant()
    {
        bool negate = false;
        if (acceptPunct("-"))
            negate = true;
        else
            acceptPunct("+");

        const Token t = take();
        Value v;
        switch (t.kind) {
            case Tok::Number:
            case Tok::String:
            case Tok::BlobLiteral: v = t.value; break;
            case Tok::Identifier:
                if (t.isKeyword("NULL"))
                    break;
                [[fallthrough]];
            default: throw Error(ErrorCode::SyntaxError, "DEFAULT needs a literal value");
        }
        if (!negate)
            return v;
        if (v.type() == Type::Integer)
            return Value(-v.integer());
        if (v.type() == Type::Real)
            return Value(-v.real());
        throw Error(ErrorCode::SyntaxError, "'-' needs a number");
    }

    Statement createIndex(bool unique)
    {
        CreateIndexStmt s;
        s.unique = unique;
        s.ifNotExists = acceptIfNotExists();
        s.name = freshName();
        expectKeyword("ON");
        s.table = freshName();
        expectPunct("(");
        do {
            s.columns.push_back(identifier());
        } while (acceptPunct(","));
        expectPunct(")");
        if (s.columns.empty())
            fail("an index needs at least one column");
        return s;
    }

    Statement insert()
    {
        InsertStmt s;
        expectKeyword("INTO");
        s.table = freshName();
        if (acceptPunct("(")) {
            do {
                s.columns.push_back(identifier());
            } while (acceptPunct(","));
            expectPunct(")");
        }
        expectKeyword("VALUES");
        do {
            expectPunct("(");
            std::vector<ExprPtr> row;
            if (!cur().isPunct(")")) {
                do {
                    row.push_back(expression());
                } while (acceptPunct(","));
            }
            expectPunct(")");
            s.rows.push_back(std::move(row));
        } while (acceptPunct(","));
        return s;
    }

    Statement select()
    {
        SelectStmt s;
        do {
            SelectStmt::Item item;
            if (cur().isPunct("*")) {
                ++at_;
                item.star = true;
            } else if (cur().kind == Tok::Identifier && peek().isPunct(".") &&
                       peek(2).isPunct("*")) {
                item.star = true;
                item.starTable = take().text;
                at_ += 2;
            } else {
                item.expr = expression();
                if (acceptKeyword("AS"))
                    item.alias = freshName();
                else if (!isReserved(cur()) && cur().kind == Tok::Identifier)
                    item.alias = take().text;
                item.aliased = !item.alias.empty();
                if (item.alias.empty())
                    item.alias = item.expr->describe();
            }
            s.items.push_back(std::move(item));
        } while (acceptPunct(","));

        expectKeyword("FROM");
        s.sources.push_back(source());
        for (;;) {
            const std::size_t mark = at_;
            if (acceptKeyword("INNER"))
                expectKeyword("JOIN");
            else if (acceptKeyword("CROSS"))
                expectKeyword("JOIN");
            else if (!acceptKeyword("JOIN")) {
                at_ = mark;
                break;
            }
            SelectStmt::Source src = source();
            if (acceptKeyword("ON"))
                src.on = expression();
            s.sources.push_back(std::move(src));
        }
        if (cur().isKeyword("LEFT") || cur().isKeyword("RIGHT") || cur().isKeyword("FULL") ||
            cur().isKeyword("OUTER"))
            throw Error(ErrorCode::Unsupported, "only INNER JOIN is supported");
        if (acceptPunct(","))
            throw Error(ErrorCode::Unsupported, "comma joins are not supported; use INNER JOIN");

        if (acceptKeyword("WHERE"))
            s.where = expression();
        if (acceptKeyword("GROUP")) {
            expectKeyword("BY");
            do {
                s.groupBy.push_back(expression());
            } while (acceptPunct(","));
        }
        if (acceptKeyword("HAVING"))
            s.having = expression();
        if (acceptKeyword("ORDER")) {
            expectKeyword("BY");
            do {
                SelectStmt::OrderTerm term;
                term.expr = expression();
                if (acceptKeyword("DESC"))
                    term.descending = true;
                else
                    acceptKeyword("ASC");
                s.order.push_back(std::move(term));
            } while (acceptPunct(","));
        }
        if (acceptKeyword("LIMIT"))
            s.limit = countLiteral("LIMIT");
        if (acceptKeyword("OFFSET"))
            s.offset = countLiteral("OFFSET");
        if (acceptKeyword("FOR"))
            s.forJson = forJson();
        return s;
    }

    /// `FOR JSON {AUTO|PATH} [, ROOT[('name')]] [, INCLUDE_NULL_VALUES]
    /// [, WITHOUT_ARRAY_WRAPPER]`, with the `FOR` already consumed.
    JsonOptions forJson()
    {
        expectKeyword("JSON");
        JsonOptions options;
        if (acceptKeyword("AUTO"))
            options.mode(JsonMode::Auto);
        else if (acceptKeyword("PATH"))
            options.mode(JsonMode::Path);
        else
            fail("expected AUTO or PATH after FOR JSON");

        while (acceptPunct(",")) {
            if (acceptKeyword("ROOT")) {
                std::string name;
                if (acceptPunct("(")) {
                    if (cur().kind != Tok::String)
                        fail("ROOT needs a quoted name");
                    name = take().value.text();
                    expectPunct(")");
                }
                options.root(name);
            } else if (acceptKeyword("INCLUDE_NULL_VALUES")) {
                options.includeNullValues();
            } else if (acceptKeyword("WITHOUT_ARRAY_WRAPPER")) {
                options.withoutArrayWrapper();
            } else {
                fail("expected ROOT, INCLUDE_NULL_VALUES or WITHOUT_ARRAY_WRAPPER");
            }
        }
        if (options.hasRoot() && options.omitsArrayWrapper())
            throw Error(ErrorCode::InvalidArgument,
                        "ROOT and WITHOUT_ARRAY_WRAPPER cannot be combined");
        return options;
    }

    std::int64_t countLiteral(const char* clause)
    {
        if (cur().kind != Tok::Number || cur().value.type() != Type::Integer)
            fail(std::string(clause) + " needs a whole number");
        const std::int64_t n = take().value.integer();
        if (n < 0)
            throw Error(ErrorCode::InvalidArgument,
                        std::string(clause) + " cannot be negative");
        return n;
    }

    SelectStmt::Source source()
    {
        SelectStmt::Source src;
        src.table = freshName();
        if (acceptKeyword("AS"))
            src.alias = freshName();
        else if (cur().kind == Tok::Identifier && !isReserved(cur()))
            src.alias = take().text;
        if (src.alias.empty())
            src.alias = src.table;
        return src;
    }

    Statement update()
    {
        UpdateStmt s;
        s.table = freshName();
        expectKeyword("SET");
        do {
            std::string column = identifier();
            if (!acceptPunct("=") && !acceptPunct("=="))
                fail("expected '=' after '" + column + "'");
            s.assignments.emplace_back(std::move(column), expression());
        } while (acceptPunct(","));
        if (acceptKeyword("WHERE"))
            s.where = expression();
        return s;
    }

    Statement remove()
    {
        DeleteStmt s;
        expectKeyword("FROM");
        s.table = freshName();
        if (acceptKeyword("WHERE"))
            s.where = expression();
        return s;
    }

    // --- expressions, loosest binding first ---
    ExprPtr expression()
    {
        const NestingGuard guard(nesting_);
        return orExpr();
    }

    ExprPtr orExpr()
    {
        ExprPtr left = andExpr();
        while (acceptKeyword("OR"))
            left = combine(BinaryOp::Or, std::move(left), andExpr());
        return left;
    }

    ExprPtr andExpr()
    {
        ExprPtr left = notExpr();
        while (acceptKeyword("AND"))
            left = combine(BinaryOp::And, std::move(left), notExpr());
        return left;
    }

    ExprPtr notExpr()
    {
        if (acceptKeyword("NOT")) {
            const NestingGuard guard(nesting_);
            auto e = node(ExprKind::Unary);
            e->unary = UnaryOp::Not;
            e->lhs = notExpr();
            return settle(std::move(e));
        }
        return comparison();
    }

    ExprPtr comparison()
    {
        ExprPtr left = additive();
        for (;;) {
            if (acceptKeyword("IS")) {
                auto e = node(ExprKind::IsNull);
                e->negated = acceptKeyword("NOT");
                expectKeyword("NULL");
                e->lhs = std::move(left);
                left = settle(std::move(e));
                continue;
            }

            bool negated = false;
            const std::size_t mark = at_;
            if (acceptKeyword("NOT"))
                negated = true;

            if (acceptKeyword("IN")) {
                auto e = node(ExprKind::InList);
                e->negated = negated;
                e->lhs = std::move(left);
                expectPunct("(");
                if (!cur().isPunct(")")) {
                    do {
                        e->list.push_back(expression());
                    } while (acceptPunct(","));
                }
                expectPunct(")");
                left = settle(std::move(e));
                continue;
            }
            if (acceptKeyword("BETWEEN")) {
                auto e = node(ExprKind::Between);
                e->negated = negated;
                e->lhs = std::move(left);
                e->list.push_back(additive());
                expectKeyword("AND");
                e->list.push_back(additive());
                left = settle(std::move(e));
                continue;
            }
            if (acceptKeyword("LIKE")) {
                auto e = node(ExprKind::Like);
                e->negated = negated;
                e->lhs = std::move(left);
                e->rhs = additive();
                left = settle(std::move(e));
                continue;
            }
            if (negated) {  // a plain NOT that belongs to the enclosing rule
                at_ = mark;
                return left;
            }

            const std::optional<BinaryOp> op = comparisonOp();
            if (!op)
                return left;
            ++at_;
            left = combine(*op, std::move(left), additive());
        }
    }

    std::optional<BinaryOp> comparisonOp() const
    {
        if (cur().kind != Tok::Punct)
            return std::nullopt;
        const std::string& t = cur().text;
        if (t == "=" || t == "==")
            return BinaryOp::Eq;
        if (t == "!=" || t == "<>")
            return BinaryOp::Ne;
        if (t == "<")
            return BinaryOp::Lt;
        if (t == "<=")
            return BinaryOp::Le;
        if (t == ">")
            return BinaryOp::Gt;
        if (t == ">=")
            return BinaryOp::Ge;
        return std::nullopt;
    }

    ExprPtr additive()
    {
        ExprPtr left = multiplicative();
        for (;;) {
            BinaryOp op;
            if (cur().isPunct("+"))
                op = BinaryOp::Add;
            else if (cur().isPunct("-"))
                op = BinaryOp::Sub;
            else if (cur().isPunct("||"))
                op = BinaryOp::Concat;
            else
                return left;
            ++at_;
            left = combine(op, std::move(left), multiplicative());
        }
    }

    ExprPtr multiplicative()
    {
        ExprPtr left = unary();
        for (;;) {
            BinaryOp op;
            if (cur().isPunct("*"))
                op = BinaryOp::Mul;
            else if (cur().isPunct("/"))
                op = BinaryOp::Div;
            else if (cur().isPunct("%"))
                op = BinaryOp::Mod;
            else
                return left;
            ++at_;
            left = combine(op, std::move(left), unary());
        }
    }

    ExprPtr unary()
    {
        if (cur().isPunct("-") || cur().isPunct("+")) {
            const NestingGuard guard(nesting_);
            auto e = node(ExprKind::Unary);
            e->unary = take().text == "-" ? UnaryOp::Negate : UnaryOp::Plus;
            e->lhs = unary();
            return settle(std::move(e));
        }
        return primary();
    }

    ExprPtr primary()
    {
        if (acceptPunct("(")) {
            ExprPtr inner = expression();
            expectPunct(")");
            return inner;
        }
        const Token& t = cur();
        switch (t.kind) {
            case Tok::Number:
            case Tok::String:
            case Tok::BlobLiteral: {
                auto e = node(ExprKind::Literal);
                e->literal = take().value;
                return e;
            }
            case Tok::Parameter: {
                ++at_;
                auto e = node(ExprKind::Parameter);
                e->parameter = params_++;
                return e;
            }
            case Tok::Identifier: {
                if (t.isKeyword("NULL")) {
                    ++at_;
                    return node(ExprKind::Literal);
                }
                if (t.isKeyword("TRUE") || t.isKeyword("FALSE")) {
                    auto e = node(ExprKind::Literal);
                    e->literal = Value(take().isKeyword("TRUE"));
                    return e;
                }
                if (peek().isPunct("("))
                    return call();
                auto e = node(ExprKind::Column);
                e->column = take().text;
                if (acceptPunct(".")) {
                    e->table = std::move(e->column);
                    e->column = identifier();
                }
                return e;
            }
            default: break;
        }
        fail("expected a value");
    }

    /// The only functions are the aggregates, so a call is always one of them.
    ExprPtr call()
    {
        const std::string name = take().text;
        static const struct
        {
            const char* spelling;
            AggregateOp op;
        } kAggregates[] = {
            {"COUNT", AggregateOp::Count}, {"SUM", AggregateOp::Sum}, {"AVG", AggregateOp::Avg},
            {"MIN", AggregateOp::Min},     {"MAX", AggregateOp::Max},
        };
        auto e = node(ExprKind::Aggregate);
        bool known = false;
        for (const auto& entry : kAggregates) {
            if (equalsNoCase(name, entry.spelling)) {
                e->aggregate = entry.op;
                known = true;
                break;
            }
        }
        if (!known)
            throw Error(ErrorCode::Unsupported, "unknown function: " + name);

        expectPunct("(");
        if (e->aggregate == AggregateOp::Count && acceptPunct("*")) {
            expectPunct(")");  // COUNT(*) counts rows, so it has no argument
            return e;
        }
        if (acceptKeyword("DISTINCT"))
            throw Error(ErrorCode::Unsupported, "DISTINCT inside an aggregate is not supported");
        e->lhs = expression();
        expectPunct(")");
        return settle(std::move(e));
    }

    static ExprPtr combine(BinaryOp op, ExprPtr lhs, ExprPtr rhs)
    {
        auto e = node(ExprKind::Binary);
        e->binary = op;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        return settle(std::move(e));
    }

    /// Records a node's height from its children and refuses one that is too tall.
    static ExprPtr settle(ExprPtr e)
    {
        unsigned tallest = 0;
        if (e->lhs)
            tallest = e->lhs->height;
        if (e->rhs)
            tallest = std::max(tallest, e->rhs->height);
        for (const ExprPtr& child : e->list)
            tallest = std::max(tallest, child->height);
        e->height = tallest + 1;
        if (e->height > kMaxExprHeight)
            throw Error(ErrorCode::SyntaxError, "expression is too deeply nested");

        return e;
    }

    static std::string_view checked(std::string_view text)
    {
        if (text.size() > kMaxStatementBytes)
            throw Error(ErrorCode::SyntaxError, "statement is longer than 1 MiB");

        return text;
    }

    /// Depth of expression() / NOT / sign recursion in the parser's own frames.
    struct NestingGuard
    {
        explicit NestingGuard(std::size_t& depth) : depth_(depth)
        {
            if (++depth_ > kMaxNesting)
                throw Error(ErrorCode::SyntaxError, "expression is nested too deeply");
        }
        ~NestingGuard() { --depth_; }
        NestingGuard(const NestingGuard&) = delete;
        NestingGuard& operator=(const NestingGuard&) = delete;
        std::size_t& depth_;
    };
    static constexpr std::size_t kMaxNesting = 128;
    static constexpr std::size_t kMaxStatementBytes = 1u << 20;
    static constexpr std::size_t kMaxTokens = 262144;
    std::vector<Token> toks_;
    std::size_t at_ = 0;
    std::size_t params_ = 0;
    std::size_t nesting_ = 0;
};

}  // namespace

std::string Expr::describe() const
{
    switch (kind) {
        case ExprKind::Literal: return literal.toLiteral();
        case ExprKind::Column: return table.empty() ? column : table + "." + column;
        case ExprKind::Parameter: return "?";
        case ExprKind::Unary:
            switch (unary) {
                case UnaryOp::Negate: return "-" + lhs->describe();
                case UnaryOp::Plus: return "+" + lhs->describe();
                case UnaryOp::Not: return "NOT " + lhs->describe();
            }
            return "?";
        case ExprKind::Binary: return lhs->describe() + spell(binary) + rhs->describe();
        case ExprKind::IsNull:
            return lhs->describe() + (negated ? " IS NOT NULL" : " IS NULL");
        case ExprKind::InList: return lhs->describe() + (negated ? " NOT IN (...)" : " IN (...)");
        case ExprKind::Between:
            return lhs->describe() + (negated ? " NOT BETWEEN " : " BETWEEN ") +
                   list[0]->describe() + " AND " + list[1]->describe();
        case ExprKind::Like:
            return lhs->describe() + (negated ? " NOT LIKE " : " LIKE ") + rhs->describe();
        case ExprKind::Aggregate:
            return std::string(toString(aggregate)) + "(" + (lhs ? lhs->describe() : "*") + ")";
    }
    return "?";
}

std::vector<Statement> parse(std::string_view text, std::size_t* parameters)
{
    Parser parser(text);
    std::vector<Statement> out = parser.all();
    if (parameters)
        *parameters = parser.parameterCount();
    return out;
}

}  // namespace sql::internal
