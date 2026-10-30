// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Recursive-descent parser for the SQLite grammar (see sqlite.org/lang.html)
// plus libsql's trailing `FOR JSON` clause on SELECT.
//
// Keywords are not a token kind: every bare word is an identifier, and the
// parser asks `isKeyword("SELECT")` where a keyword may appear. SQLite lets
// most keywords double as names ("fallback"); the few that may not are listed
// in isReservedWord().

#include "internal/parser.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_set>

#include "internal/func.hpp"
#include "internal/lexer.hpp"

namespace sql::internal {

bool mutates(const StatementNode& s) noexcept
{
    switch (s.body.index()) {
        case 0:   // SELECT
        case 10:  // transaction control
        case 11:  // PRAGMA (decided at run time)
        case 13:  // DETACH
        case 17:  // EXPLAIN never runs its statement
            return false;
        default: return true;
    }
}

bool controlsTransaction(const StatementNode& s) noexcept
{
    return std::holds_alternative<TransactionStmt>(s.body);
}

const char* spell(BinaryOp op) noexcept
{
    switch (op) {
        case BinaryOp::Or: return "OR";
        case BinaryOp::And: return "AND";
        case BinaryOp::Eq: return "=";
        case BinaryOp::Ne: return "<>";
        case BinaryOp::Lt: return "<";
        case BinaryOp::Le: return "<=";
        case BinaryOp::Gt: return ">";
        case BinaryOp::Ge: return ">=";
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
        case BinaryOp::Concat: return "||";
        case BinaryOp::BitAnd: return "&";
        case BinaryOp::BitOr: return "|";
        case BinaryOp::ShiftLeft: return "<<";
        case BinaryOp::ShiftRight: return ">>";
    }
    return "?";
}

namespace {

thread_local Arena* tArena = nullptr;
thread_local std::size_t tArenaBlock = 4096;

}  // namespace

void ExprDelete::operator()(Expr* e) const noexcept
{
    if (e->inArena)
        e->~Expr();
    else
        delete e;
}

ExprPtr makeExpr(ExprKind kind)
{
    Expr* e;
    if (tArena) {
        e = new (tArena->allocate(sizeof(Expr), alignof(Expr))) Expr();
        e->inArena = true;
    } else {
        e = new Expr();
    }
    e->kind = kind;
    return ExprPtr(e);
}

ArenaScope::ArenaScope(Arena* a) noexcept : saved_(tArena)
{
    tArena = a;
}

ArenaScope::~ArenaScope()
{
    tArena = saved_;
}

void setArenaBlockSize(std::size_t bytes) noexcept
{
    tArenaBlock = bytes;
}

std::size_t arenaBlockSize() noexcept
{
    return tArenaBlock;
}

std::string Expr::describe() const
{
    switch (kind) {
        case ExprKind::Literal: return literal.toLiteral();
        case ExprKind::Column:
            return (schema.empty() ? std::string() : schema + ".") +
                   (table.empty() ? std::string() : table + ".") + name;
        case ExprKind::Parameter: return "?";
        case ExprKind::Function:
        case ExprKind::Aggregate:
        case ExprKind::Window: {
            std::string s = name + "(";
            if (star)
                s += "*";
            for (std::size_t i = 0; i < list.size(); ++i)
                s += (i ? ", " : "") + list[i]->describe();
            return s + ")";
        }
        case ExprKind::Binary:
            return lhs->describe() + " " + spell(BinaryOp(op)) + " " + rhs->describe();
        default: return "expression";
    }
}

namespace {


/// Names a call may use: the registry's, the window functions', and the
/// functions REGEXP/MATCH stand for (which fail later unless provided).
bool knownFunction(std::string_view name)
{
    static const char* kWindow[] = {"row_number", "rank", "dense_rank", "percent_rank", "cume_dist",
                                    "ntile", "lag", "lead", "first_value", "last_value", "nth_value"};
    for (const char* w : kWindow) {
        if (equalsNoCase(name, w))
            return true;
    }
    return FunctionRegistry::builtins().hasName(name);
}

constexpr std::size_t kMaxNesting = 200;
constexpr std::size_t kMaxStatementBytes = 1u << 20;
constexpr std::size_t kMaxTokens = 262144;
constexpr std::size_t kMaxCompound = 500;
constexpr std::size_t kMaxVariable = 32766;

/// Token vectors parsed into and handed back, per thread: preparing a
/// statement then costs no allocation for its token list. Only modest
/// vectors are kept, so a huge statement does not pin its tokens.
std::vector<std::vector<Token>>& tokenPool()
{
    thread_local std::vector<std::vector<Token>> pool;
    return pool;
}

std::vector<Token> borrowTokens()
{
    std::vector<std::vector<Token>>& pool = tokenPool();
    if (pool.empty())
        return {};
    std::vector<Token> v = std::move(pool.back());
    pool.pop_back();
    return v;
}

void returnTokens(std::vector<Token>& v) noexcept
{
    constexpr std::size_t kKeepTokens = 4096;
    constexpr std::size_t kKeepVectors = 4;
    std::vector<std::vector<Token>>& pool = tokenPool();
    if (v.capacity() == 0 || v.capacity() > kKeepTokens || pool.size() >= kKeepVectors) {
        std::vector<Token>().swap(v);
        return;
    }
    v.clear();
    try {
        pool.push_back(std::move(v));
    } catch (...) {
    }
}

class Parser
{
public:
    explicit Parser(std::string_view text) : text_(checked(text)), toks_(borrowTokens())
    {
        try {
            tokenizeInto(text_, toks_);
        } catch (...) {
            returnTokens(toks_);
            throw;
        }
        if (toks_.size() > kMaxTokens) {
            returnTokens(toks_);
            throw Error(ErrorCode::SyntaxError, "statement has too many tokens");
        }
    }
    ~Parser() { returnTokens(toks_); }
    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    ParsedBatch all()
    {
        ParsedBatch out;
        for (;;) {
            while (acceptPunct(";")) {
            }
            if (cur().kind == Tok::End)
                break;
            out.statements.push_back(statement());
            if (cur().kind != Tok::End && !cur().isPunct(";"))
                fail();
        }
        if (out.statements.empty())
            throw Error(ErrorCode::SyntaxError, "no statement to run");
        out.parameterNames = std::move(paramNames_);
        return out;
    }

    StatementNode one()
    {
        while (acceptPunct(";")) {
        }
        StatementNode s = statement();
        while (acceptPunct(";")) {
        }
        if (cur().kind != Tok::End)
            fail();
        return s;
    }

    ExprPtr standaloneExpression()
    {
        ExprPtr e = expr();
        if (cur().kind != Tok::End)
            fail();
        return e;
    }

private:
    // ------------------------------------------------- token plumbing ---
    const Token& cur() const { return toks_[at_]; }
    const Token& peek(std::size_t n = 1) const
    {
        return toks_[std::min(at_ + n, toks_.size() - 1)];
    }
    const Token& take() { return toks_[at_ < toks_.size() - 1 ? at_++ : at_]; }
    std::uint32_t prevEnd() const
    {
        if (at_ == 0)
            return 0;
        const Token& t = toks_[at_ - 1];
        return t.offset + t.length;
    }

    /// SQLite's message: `near "X": syntax error`.
    [[noreturn]] void fail() const
    {
        if (cur().kind == Tok::End)
            throw Error(ErrorCode::SyntaxError, "incomplete input");
        const Token& t = cur();
        throw Error(ErrorCode::SyntaxError,
                    "near \"" + std::string(text_.substr(t.offset, t.length)) + "\": syntax error");
    }
    [[noreturn]] void failWith(const std::string& what) const
    {
        throw Error(ErrorCode::SyntaxError, what);
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
            fail();
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
            fail();
    }

    bool isBareReserved(const Token& t) const
    {
        return t.kind == Tok::Identifier && !t.quoted && t.reserved;
    }

    /// A name: any identifier that is not reserved, or a string literal
    /// (SQLite accepts 'x' as a name in most places).
    std::string name()
    {
        const Token& t = cur();
        if (t.kind == Tok::Identifier && !isBareReserved(t)) {
            ++at_;
            return t.text;
        }
        if (t.kind == Tok::String) {
            ++at_;
            return std::string(t.value.text());
        }
        fail();
    }
    bool atName() const
    {
        const Token& t = cur();
        return (t.kind == Tok::Identifier && !isBareReserved(t)) || t.kind == Tok::String;
    }

    /// `[schema.]name`, storing the offset of `name` in `nameBegin` if given.
    void qualifiedName(std::string& schema, std::string& nm, std::uint32_t* nameBegin = nullptr)
    {
        std::uint32_t at = cur().offset;
        nm = name();
        if (acceptPunct(".")) {
            schema = std::move(nm);
            at = cur().offset;
            nm = name();
        }
        if (nameBegin)
            *nameBegin = at;
    }

    bool acceptIfNotExists()
    {
        if (!cur().isKeyword("IF") || !peek().isKeyword("NOT"))
            return false;
        at_ += 2;
        expectKeyword("EXISTS");
        return true;
    }
    bool acceptIfExists()
    {
        if (!cur().isKeyword("IF") || !peek().isKeyword("EXISTS"))
            return false;
        at_ += 2;
        return true;
    }

    void enter()
    {
        if (++depth_ > kMaxNesting)
            failWith("parser stack overflow");
    }
    void leave() { --depth_; }

    static std::string_view checked(std::string_view text)
    {
        if (text.size() > kMaxStatementBytes)
            throw Error(ErrorCode::SyntaxError, "statement too long");
        return text;
    }

    // ----------------------------------------------------- statements ---
    StatementNode statement()
    {
        StatementNode node;
        node.arena = std::make_unique<Arena>(tArenaBlock);
        const ArenaScope scope(node.arena.get());
        node.begin = cur().offset;
        node.body = statementBody();
        node.end = prevEnd();
        return node;
    }

    StatementBody statementBody()
    {
        const Token& t = cur();
        if (t.isKeyword("EXPLAIN")) {
            ++at_;
            ExplainStmt e;
            if (cur().isKeyword("QUERY")) {
                ++at_;
                expectKeyword("PLAN");
                e.queryPlan = true;
            }
            e.statement = std::make_unique<StatementNode>(statement());
            return e;
        }
        if (t.isKeyword("WITH")) {
            auto with = withClause();
            if (cur().isKeyword("SELECT") || cur().isKeyword("VALUES"))
                return selectAfterWith(std::move(with));
            if (cur().isKeyword("INSERT") || cur().isKeyword("REPLACE"))
                return insert(std::move(with));
            if (cur().isKeyword("UPDATE"))
                return update(std::move(with));
            if (cur().isKeyword("DELETE"))
                return remove(std::move(with));
            fail();
        }
        if (t.isKeyword("SELECT") || t.isKeyword("VALUES"))
            return selectStatement();
        if (t.isKeyword("INSERT") || t.isKeyword("REPLACE"))
            return insert(nullptr);
        if (t.isKeyword("UPDATE"))
            return update(nullptr);
        if (t.isKeyword("DELETE"))
            return remove(nullptr);
        if (t.isKeyword("CREATE"))
            return create();
        if (t.isKeyword("DROP"))
            return drop();
        if (t.isKeyword("ALTER"))
            return alter();
        if (t.isKeyword("BEGIN") || t.isKeyword("COMMIT") || t.isKeyword("END") ||
            t.isKeyword("ROLLBACK") || t.isKeyword("SAVEPOINT") || t.isKeyword("RELEASE"))
            return transaction();
        if (t.isKeyword("PRAGMA"))
            return pragma();
        if (t.isKeyword("ATTACH")) {
            ++at_;
            acceptKeyword("DATABASE");
            AttachStmt a;
            a.file = expr();
            expectKeyword("AS");
            a.schema = expr();
            if (acceptKeyword("KEY"))
                (void)expr();
            return a;
        }
        if (t.isKeyword("DETACH")) {
            ++at_;
            acceptKeyword("DATABASE");
            DetachStmt d;
            d.schema = expr();
            return d;
        }
        if (t.isKeyword("ANALYZE")) {
            ++at_;
            AnalyzeStmt a;
            if (atName())
                qualifiedName(a.schema, a.name);
            return a;
        }
        if (t.isKeyword("REINDEX")) {
            ++at_;
            ReindexStmt r;
            if (atName())
                qualifiedName(r.schema, r.name);
            return r;
        }
        if (t.isKeyword("VACUUM")) {
            ++at_;
            VacuumStmt v;
            if (atName() && !cur().isKeyword("INTO"))
                v.schema = name();
            if (acceptKeyword("INTO"))
                v.into = expr();
            return v;
        }
        fail();
    }

    TransactionStmt transaction()
    {
        TransactionStmt s;
        if (acceptKeyword("BEGIN")) {
            s.kind = TransactionStmt::Kind::Begin;
            if (acceptKeyword("DEFERRED"))
                s.mode = 0;
            else if (acceptKeyword("IMMEDIATE"))
                s.mode = 1;
            else if (acceptKeyword("EXCLUSIVE"))
                s.mode = 2;
            if (acceptKeyword("TRANSACTION") && atName())
                s.name = name();
            return s;
        }
        if (acceptKeyword("COMMIT") || acceptKeyword("END")) {
            s.kind = TransactionStmt::Kind::Commit;
            if (acceptKeyword("TRANSACTION") && atName())
                s.name = name();
            return s;
        }
        if (acceptKeyword("ROLLBACK")) {
            s.kind = TransactionStmt::Kind::Rollback;
            if (acceptKeyword("TRANSACTION") && atName() && !cur().isKeyword("TO"))
                s.name = name();
            if (acceptKeyword("TO")) {
                acceptKeyword("SAVEPOINT");
                s.kind = TransactionStmt::Kind::RollbackTo;
                s.name = name();
            }
            return s;
        }
        if (acceptKeyword("SAVEPOINT")) {
            s.kind = TransactionStmt::Kind::Savepoint;
            s.name = name();
            return s;
        }
        expectKeyword("RELEASE");
        acceptKeyword("SAVEPOINT");
        s.kind = TransactionStmt::Kind::Release;
        s.name = name();
        return s;
    }

    PragmaStmt pragma()
    {
        expectKeyword("PRAGMA");
        PragmaStmt p;
        qualifiedName(p.schema, p.name);
        if (acceptPunct("=")) {
            p.value = pragmaValue();
        } else if (acceptPunct("(")) {
            p.value = pragmaValue();
            expectPunct(")");
        }
        return p;
    }

    ExprPtr pragmaValue()
    {
        const Token& t = cur();
        if (t.isPunct("-") || t.isPunct("+")) {
            const bool neg = t.isPunct("-");
            ++at_;
            if (cur().kind != Tok::Number)
                fail();
            ExprPtr e = makeExpr(ExprKind::Literal);
            const Value& v = take().value;
            if (!neg)
                e->literal = v;
            else if (v.type() == Type::Integer)
                e->literal = Value(-v.integer());
            else
                e->literal = Value(-v.real());
            return e;
        }
        if (t.kind == Tok::Number || t.kind == Tok::String) {
            ExprPtr e = makeExpr(ExprKind::Literal);
            e->literal = take().value;
            return e;
        }
        if (t.kind == Tok::Identifier) {
            // ON, OFF, YES, NO, FULL, a table name...: a word, kept as text.
            ExprPtr e = makeExpr(ExprKind::Literal);
            e->literal = Value(take().text);
            return e;
        }
        fail();
    }

    // ---------------------------------------------------------- WITH ----
    std::unique_ptr<WithClause> withClause()
    {
        expectKeyword("WITH");
        auto w = std::make_unique<WithClause>();
        w->recursive = acceptKeyword("RECURSIVE");
        do {
            CommonTableExpr cte;
            cte.name = name();
            if (acceptPunct("(")) {
                do
                    cte.columns.push_back(name());
                while (acceptPunct(","));
                expectPunct(")");
            }
            expectKeyword("AS");
            if (acceptKeyword("MATERIALIZED")) {
                cte.materialized = 1;
            } else if (cur().isKeyword("NOT") && peek().isKeyword("MATERIALIZED")) {
                at_ += 2;
                cte.materialized = 2;
            }
            expectPunct("(");
            enter();
            cte.select = std::make_unique<SelectStmt>(selectStatement());
            leave();
            expectPunct(")");
            w->ctes.push_back(std::move(cte));
        } while (acceptPunct(","));
        return w;
    }

    /// A WITH on INSERT applies to the statement's SELECT (or VALUES). When
    /// that SELECT has a WITH of its own, its tables shadow the outer ones.
    static void attachWith(SelectStmt& sel, std::unique_ptr<WithClause> with)
    {
        if (!with)
            return;
        if (sel.with) {
            for (CommonTableExpr& inner : sel.with->ctes) {
                auto& outer = with->ctes;
                outer.erase(std::remove_if(outer.begin(), outer.end(),
                                           [&](const CommonTableExpr& o) { return equalsNoCase(o.name, inner.name); }),
                            outer.end());
                outer.push_back(std::move(inner));
            }
            with->recursive = with->recursive || sel.with->recursive;
        }
        sel.with = std::move(with);
    }

#include "internal/parser_select.inc"
#include "internal/parser_expr.inc"
#include "internal/parser_dml.inc"
#include "internal/parser_ddl.inc"

    std::string_view text_;
    std::vector<Token> toks_;
    std::size_t at_ = 0;
    std::size_t depth_ = 0;
    std::size_t maxParam_ = 0;
    std::vector<std::string> paramNames_;
    std::map<std::string, std::size_t> namedParams_;
};

}  // namespace

ParsedBatch parse(std::string_view text)
{
    Parser p(text);
    return p.all();
}

StatementNode parseOne(std::string_view text)
{
    Parser p(text);
    return p.one();
}

ExprPtr parseExpression(std::string_view text)
{
    Parser p(text);
    return p.standaloneExpression();
}

}  // namespace sql::internal
