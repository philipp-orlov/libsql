// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The function registry: every scalar, aggregate and table-valued function the
// SQL layer can call, resolved by name and argument count when a statement is
// prepared, so that running it costs one indirect call.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "internal/types.hpp"
#include "sql/error.hpp"
#include "sql/value.hpp"

namespace sql::internal {

struct ConnectionState;

/// What a call can see besides its arguments. One context serves a whole
/// statement execution; functions must not keep pointers into it.
struct FuncContext
{
    /// The connection running the statement; null when a function is called
    /// outside one (unit tests, schema validation).
    ConnectionState* conn = nullptr;
    /// The collation in effect for this call, for functions that compare text
    /// (min, max, instr, ...). Set by the caller before each call.
    Collation collation = Collation::Binary;
    /// Statement time in microseconds since the epoch, captured on first use
    /// and kept for the rest of the statement execution, so `'now'` is stable
    /// within a statement as SQLite promises. The executor resets it.
    std::int64_t nowUsec = INT64_MIN;
    /// Scratch a function may use freely; its contents are unspecified on
    /// entry. Keeping it here lets text-producing functions stop allocating
    /// once it has grown.
    std::string buf;
    std::string buf2;

    /// Current statement time (see nowUsec).
    std::int64_t now();

    /// Raises the SQLite-style error `message` (ErrorCode::Runtime).
    [[noreturn]] void error(const std::string& message) const;
    /// "string or blob too big".
    [[noreturn]] void tooBig() const;
};

/// Arguments: argc pointers to values that live at least for the call.
using Args = const Value* const*;

/// A scalar function writes its result to `out`, which may hold anything on
/// entry (and keeps its buffers). It must not read `out` as an argument.
using ScalarFn = void (*)(FuncContext& ctx, int argc, Args argv, Value& out);

/// The running state of one aggregate over one group (or one window frame).
class AggregateState
{
public:
    virtual ~AggregateState() = default;
    virtual void step(FuncContext& ctx, int argc, Args argv) = 0;
    /// Undo a step() made earlier with the same arguments; called only for
    /// functions flagged kInvertible, as a window frame slides past a row.
    virtual void inverse(FuncContext& ctx, int argc, Args argv);
    /// The result so far. Does not end the aggregate: window functions call it
    /// once per row.
    virtual void value(FuncContext& ctx, Value& out) = 0;
    /// Back to the empty state, keeping any buffers.
    virtual void reset() = 0;
};

using AggregateFactory = std::unique_ptr<AggregateState> (*)();

enum FunctionFlag : unsigned
{
    kDeterministic = 1u << 0,   ///< same arguments, same result: allowed in indexes, CHECK, generated columns
    kInvertible = 1u << 1,      ///< the aggregate implements inverse()
    kNeedsCollation = 1u << 2,  ///< reads ctx.collation
    kSubtypeAware = 1u << 3,    ///< reads or sets value subtypes (JSON)
};

struct FunctionDef
{
    std::string name;  ///< lowercase
    int minArgs = 0;
    int maxArgs = 0;  ///< -1 for any number
    unsigned flags = kDeterministic;
    ScalarFn scalar = nullptr;           ///< exactly one of scalar and aggregate is set
    AggregateFactory aggregate = nullptr;
};

/// One scan of a table-valued function such as json_each.
class TableFunctionCursor
{
public:
    virtual ~TableFunctionCursor() = default;
    /// Starts a scan. `argv` are the function's arguments, which bind the
    /// hidden columns in order.
    virtual void open(FuncContext& ctx, int argc, Args argv) = 0;
    virtual bool eof() const = 0;
    virtual void next(FuncContext& ctx) = 0;
    /// Column `i` of the current row, numbered as TableFunctionDef::columns.
    virtual void column(FuncContext& ctx, int i, Value& out) = 0;
    virtual std::int64_t rowid() const = 0;
};

struct TableFunctionDef
{
    std::string name;                  ///< lowercase
    std::vector<std::string> columns;  ///< every column: the visible ones, then the hidden ones
    int hiddenStart = 0;               ///< columns[hiddenStart..] are hidden; arguments bind them in order
    int minArgs = 0;
    int maxArgs = 0;
    std::unique_ptr<TableFunctionCursor> (*make)() = nullptr;
};

class FunctionRegistry
{
public:
    void add(FunctionDef def);
    void addTable(TableFunctionDef def);

    /// The function called `name` (any case) that takes `argc` arguments,
    /// preferring an exact arity over a variadic one; null if none.
    const FunctionDef* find(std::string_view name, int argc) const;
    /// True if some arity of `name` exists, which tells "wrong number of
    /// arguments" from "no such function".
    bool hasName(std::string_view name) const;
    const TableFunctionDef* findTable(std::string_view name) const;

    const std::vector<FunctionDef>& all() const noexcept { return functions_; }
    const std::vector<TableFunctionDef>& tables() const noexcept { return tables_; }

    /// Every built-in function, registered once per process.
    static const FunctionRegistry& builtins();

private:
    std::vector<FunctionDef> functions_;
    std::vector<TableFunctionDef> tables_;
    std::unordered_map<std::string, std::vector<std::size_t>> byName_;
};

// Registration entry points, one per module.
void registerCoreFunctions(FunctionRegistry& r);       // func_core.cpp
void registerMathFunctions(FunctionRegistry& r);       // func_core.cpp
void registerAggregateFunctions(FunctionRegistry& r);  // func_aggregate.cpp
void registerJsonFunctions(FunctionRegistry& r);       // func_json.cpp
void registerDateFunctions(FunctionRegistry& r);       // func_date.cpp
void registerPrintfFunctions(FunctionRegistry& r);     // func_printf.cpp

/// SQLite's printf, shared by printf()/format() and anything else that wants
/// it. `argv` supplies the arguments for the conversions after the format.
void sqlitePrintf(FuncContext& ctx, std::string_view format, int argc, Args argv,
                  std::string& out);

/// SQL literal text for a value, as quote() renders it.
void quoteValue(const Value& v, std::string& out);

}  // namespace sql::internal
