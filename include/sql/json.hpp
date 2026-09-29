// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libsql -- rendering a Result as JSON, in the shapes T-SQL's FOR JSON clause
// produces.
//
// The same machinery backs the SQL-level clause:
//
//     SELECT ... FOR JSON AUTO
//     SELECT ... FOR JSON PATH, ROOT('orders'), INCLUDE_NULL_VALUES
//
// and the C++ API below, which renders a Result that was produced any other
// way. Both are written for processes that stay up for months: the writer
// keeps its buffers between calls and sizes the output exactly before filling
// it, so a loop that reuses one JsonWriter settles at zero allocations per
// query instead of churning the heap.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sql/sql.hpp"

namespace sql {

/// How column names map onto the JSON tree.
enum class JsonMode : std::uint8_t
{
    /// `FOR JSON AUTO`: a column named `a.b.c` belongs to an object `b` nested
    /// in an array named `b` inside an object `a`, and consecutive rows that
    /// agree on every enclosing column collapse into one element with the
    /// differing rows gathered in the nested array -- the shape a joined query
    /// wants. Names without a dot stay at the top level.
    Auto,
    /// `FOR JSON PATH`: the dots in a column name spell the path to it and
    /// every row becomes one object, so `SELECT c.name AS "customer.name"`
    /// yields `{"customer":{"name":...}}`. Nothing is collapsed.
    Path,
};

/// The FOR JSON modifiers, as a fluent builder:
/// `JsonOptions().mode(JsonMode::Path).root("orders").includeNullValues()`.
class JsonOptions
{
public:
    JsonOptions& mode(JsonMode m) noexcept
    {
        mode_ = m;
        return *this;
    }
    /// Render a NULL column as `null` instead of leaving the property out,
    /// which is what `INCLUDE_NULL_VALUES` asks for.
    JsonOptions& includeNullValues(bool on = true) noexcept
    {
        includeNulls_ = on;
        return *this;
    }
    /// Wrap the whole document in a one-property object, as `ROOT('name')`
    /// does. An empty name means `root`, the T-SQL default.
    JsonOptions& root(std::string_view name = {})
    {
        hasRoot_ = true;
        root_ = name.empty() ? "root" : std::string(name);
        return *this;
    }
    JsonOptions& noRoot() noexcept
    {
        hasRoot_ = false;
        root_.clear();
        return *this;
    }
    /// Emit the objects on their own, without the enclosing `[` and `]`. As in
    /// T-SQL this cannot be combined with a root.
    JsonOptions& withoutArrayWrapper(bool on = true) noexcept
    {
        withoutWrapper_ = on;
        return *this;
    }

    JsonMode mode() const noexcept { return mode_; }
    bool includesNullValues() const noexcept { return includeNulls_; }
    bool hasRoot() const noexcept { return hasRoot_; }
    const std::string& rootName() const noexcept { return root_; }
    bool omitsArrayWrapper() const noexcept { return withoutWrapper_; }

private:
    std::string root_;
    JsonMode mode_ = JsonMode::Auto;
    bool includeNulls_ = false;
    bool hasRoot_ = false;
    bool withoutWrapper_ = false;
};

/// A reusable renderer.
///
/// Keep one per thread (or per worker) and call it in a loop: it holds on to
/// the output buffer and to the plan it derived from the column names, reuses
/// both whenever the next result has the same shape, and grows the buffer at
/// most once per call because it measures the document before writing it.
/// The one-shot `toJson` below is the convenient version for code that does
/// not run hot.
///
/// Move-only, and not safe to use from two threads at once.
class JsonWriter
{
public:
    JsonWriter();
    explicit JsonWriter(JsonOptions options);
    ~JsonWriter();
    JsonWriter(JsonWriter&&) noexcept;
    JsonWriter& operator=(JsonWriter&&) noexcept;
    JsonWriter(const JsonWriter&) = delete;
    JsonWriter& operator=(const JsonWriter&) = delete;

    const JsonOptions& options() const noexcept;
    JsonWriter& options(JsonOptions options);

    /// Render into the writer's own buffer and return a view of it, valid
    /// until the next call on this writer.
    std::string_view write(const Result& result);
    /// The same, but with `paths` standing in for the result's column names,
    /// so a query can be re-nested without being rewritten. One entry per
    /// column; a shorter list leaves the remaining columns at the top level.
    std::string_view write(const Result& result, const std::vector<std::string>& paths);

    /// Append onto a buffer the caller owns and recycles. The buffer is grown
    /// once, to exactly what is needed, and never shrunk.
    void appendTo(const Result& result, std::string& out);
    void appendTo(const Result& result, const std::vector<std::string>& paths, std::string& out);

    /// Exactly how many bytes rendering would produce, without producing them.
    std::size_t measure(const Result& result);
    std::size_t measure(const Result& result, const std::vector<std::string>& paths);

    /// What the last `write` produced; empty before the first one.
    std::string_view text() const noexcept;
    /// Bytes the internal buffer currently holds on to.
    std::size_t capacity() const noexcept;
    /// Give that memory back, after one unusually large result would otherwise
    /// pin it for the life of the process.
    void compact();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// One-shot rendering, for code that is not in a loop.
std::string toJson(const Result& result, const JsonOptions& options = JsonOptions());
/// One-shot rendering onto the end of `out`, which is not cleared first.
void appendJson(const Result& result, std::string& out,
                const JsonOptions& options = JsonOptions());

}  // namespace sql
