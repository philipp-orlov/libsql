// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// JSON rendering. Two passes over the result: one that counts bytes and one
// that writes them, so the output buffer is grown exactly once and the writer
// can be recycled for the life of the process without fragmenting the heap.
// Nothing here allocates a temporary string -- numbers, datetimes and blobs
// are formatted into stack buffers and appended straight to the output.

#include "sql/json.hpp"

#include "internal/cnumeric.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sql {

namespace {

/// The sink both passes write through. With no string behind it, it counts;
/// with one, it appends. One perfectly predicted branch per call is cheaper
/// than keeping two copies of the emit logic in step.
class Out
{
public:
    Out() = default;
    explicit Out(std::string& s) noexcept : s_(&s) {}

    void put(char c)
    {
        if (s_)
            s_->push_back(c);
        else
            ++n_;
    }
    void put(std::string_view v)
    {
        if (s_)
            s_->append(v);
        else
            n_ += v.size();
    }
    void put(const char* p, std::size_t k)
    {
        if (s_)
            s_->append(p, k);
        else
            n_ += k;
    }

    std::size_t counted() const noexcept { return n_; }

private:
    std::string* s_ = nullptr;
    std::size_t n_ = 0;
};

void putEscaped(Out& o, std::string_view s)
{
    // Everything that needs no escaping goes out in runs; only the rare byte
    // is handled one at a time.
    std::size_t run = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c != '"' && c != '\\')
            continue;
        if (i > run)
            o.put(s.substr(run, i - run));
        switch (c) {
            case '"': o.put("\\\"", 2); break;
            case '\\': o.put("\\\\", 2); break;
            case '\b': o.put("\\b", 2); break;
            case '\f': o.put("\\f", 2); break;
            case '\n': o.put("\\n", 2); break;
            case '\r': o.put("\\r", 2); break;
            case '\t': o.put("\\t", 2); break;
            default: {
                static const char kHex[] = "0123456789abcdef";
                const char esc[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 0x0f]};
                o.put(esc, sizeof esc);
                break;
            }
        }
        run = i + 1;
    }
    if (s.size() > run)
        o.put(s.substr(run));
}

void putKey(Out& o, std::string_view name)
{
    o.put('"');
    putEscaped(o, name);
    o.put("\":", 2);
}

void putInteger(Out& o, std::int64_t v)
{
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    o.put(buf, std::size_t(r.ptr - buf));
}

void putReal(Out& o, double v)
{
    if (!std::isfinite(v)) {  // JSON has no infinity or NaN
        o.put("null", 4);
        return;
    }
    char buf[40];
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    o.put(buf, std::size_t(r.ptr - buf));
#else
    // Without std::to_chars for doubles, find the shortest of the three
    // precisions that still round-trips, the way to_chars would -- in the C
    // locale, so the output is always JSON.
    const int n = internal::formatShortest(buf, sizeof buf, v);
    o.put(buf, std::size_t(n));
#endif
}

void putDatetime(Out& o, Datetime when)
{
    char buf[32];
    const std::size_t n = formatDatetimeIso(when, buf, sizeof buf);
    o.put('"');
    o.put(buf, n);
    o.put('"');
}

void putBase64(Out& o, const Blob& bytes)
{
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto byteAt = [&](std::size_t i) {
        return std::uint32_t(std::to_integer<unsigned char>(bytes[i]));
    };

    o.put('"');
    std::size_t i = 0;
    char quad[4];
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t v = (byteAt(i) << 16) | (byteAt(i + 1) << 8) | byteAt(i + 2);
        quad[0] = kAlphabet[(v >> 18) & 0x3f];
        quad[1] = kAlphabet[(v >> 12) & 0x3f];
        quad[2] = kAlphabet[(v >> 6) & 0x3f];
        quad[3] = kAlphabet[v & 0x3f];
        o.put(quad, 4);
    }
    if (i < bytes.size()) {
        const bool two = i + 2 == bytes.size();
        const std::uint32_t v = (byteAt(i) << 16) | (two ? byteAt(i + 1) << 8 : 0);
        quad[0] = kAlphabet[(v >> 18) & 0x3f];
        quad[1] = kAlphabet[(v >> 12) & 0x3f];
        quad[2] = two ? kAlphabet[(v >> 6) & 0x3f] : '=';
        quad[3] = '=';
        o.put(quad, 4);
    }
    o.put('"');
}

void putValue(Out& o, const Value& v)
{
    switch (v.type()) {
        case Type::Null: o.put("null", 4); break;
        case Type::Integer: putInteger(o, v.integer()); break;
        case Type::Real: putReal(o, v.real()); break;
        case Type::Datetime: putDatetime(o, v.datetime()); break;
        case Type::Text:
            o.put('"');
            putEscaped(o, v.text());
            o.put('"');
            break;
        case Type::Blob: putBase64(o, v.blob()); break;
    }
}

const Value& cell(const Row& row, int column)
{
    static const Value kNull;
    const std::vector<Value>& values = row.values();
    return std::size_t(column) < values.size() ? values[std::size_t(column)] : kNull;
}

}  // namespace

// ---------------------------------------------------------------- impl ----

struct JsonWriter::Impl
{
    JsonOptions options;
    std::string buffer;

    /// One object in the tree the column names describe. `columns` are the
    /// properties that live directly on it; `children` index back into
    /// `nodes`. Node 0 is the row object itself.
    struct Node
    {
        std::string_view name;
        std::vector<int> columns;
        std::vector<int> children;
    };

    // The plan. `paths` owns the text; every string_view below points into it,
    // which is why the plan is rebuilt only when the names actually change.
    std::vector<std::string> paths;
    std::vector<Node> nodes;
    std::size_t nodeCount = 0;
    std::vector<std::string_view> property;  ///< column -> its property name
    bool planned = false;

    void plan(const std::vector<std::string>& names, std::size_t columns);
    void emit(Out& o, const Result& result) const;
    void emitAuto(Out& o, int node, std::size_t begin, std::size_t end, const Result& r) const;
    bool emitPath(Out& o, int node, const Row& row) const;
    bool anyContent(int node, const Row& row) const;
};

void JsonWriter::Impl::plan(const std::vector<std::string>& names, std::size_t columns)
{
    const auto nameAt = [&](std::size_t i) {
        return i < names.size() ? std::string_view(names[i]) : std::string_view();
    };

    bool unchanged = planned && paths.size() == columns;
    for (std::size_t i = 0; unchanged && i < columns; ++i)
        unchanged = std::string_view(paths[i]) == nameAt(i);
    if (unchanged)
        return;

    // Copying into the strings already there recycles their buffers rather
    // than freeing and reallocating one per column.
    paths.resize(columns);
    for (std::size_t i = 0; i < columns; ++i)
        paths[i].assign(nameAt(i));

    for (Node& n : nodes) {
        n.columns.clear();
        n.children.clear();
    }
    nodeCount = 0;
    const auto addNode = [&](std::string_view name) {
        if (nodeCount == nodes.size())
            nodes.emplace_back();
        nodes[nodeCount].name = name;
        return int(nodeCount++);
    };
    addNode({});  // the row object

    property.resize(columns);
    for (std::size_t i = 0; i < columns; ++i) {
        std::string_view rest = paths[i];
        int node = 0;
        for (std::size_t dot = rest.find('.'); dot != std::string_view::npos;
             dot = rest.find('.')) {
            const std::string_view segment = rest.substr(0, dot);
            rest.remove_prefix(dot + 1);
            int child = -1;
            for (const int c : nodes[node].children) {
                if (nodes[std::size_t(c)].name == segment) {
                    child = c;
                    break;
                }
            }
            if (child < 0) {
                child = addNode(segment);
                nodes[std::size_t(node)].children.push_back(child);
            }
            node = child;
        }
        property[i] = rest;
        nodes[std::size_t(node)].columns.push_back(int(i));
    }
    planned = true;
}

bool JsonWriter::Impl::anyContent(int node, const Row& row) const
{
    const Node& n = nodes[std::size_t(node)];
    for (const int c : n.columns) {
        if (!cell(row, c).isNull())
            return true;
    }
    for (const int child : n.children) {
        if (anyContent(child, row))
            return true;
    }
    return false;
}

bool JsonWriter::Impl::emitPath(Out& o, int node, const Row& row) const
{
    const bool includeNulls = options.includesNullValues();
    const Node& n = nodes[std::size_t(node)];

    o.put('{');
    bool member = false;
    for (const int c : n.columns) {
        const Value& v = cell(row, c);
        if (v.isNull() && !includeNulls)
            continue;
        if (member)
            o.put(',');
        member = true;
        putKey(o, property[std::size_t(c)]);
        putValue(o, v);
    }
    for (const int child : n.children) {
        // A nested object whose every column is NULL is left out entirely,
        // unless nulls were asked for.
        if (!includeNulls && !anyContent(child, row))
            continue;
        if (member)
            o.put(',');
        member = true;
        putKey(o, nodes[std::size_t(child)].name);
        emitPath(o, child, row);
    }
    o.put('}');
    return member;
}

void JsonWriter::Impl::emitAuto(Out& o, int node, std::size_t begin, std::size_t end,
                                const Result& r) const
{
    const bool includeNulls = options.includesNullValues();
    const Node& n = nodes[std::size_t(node)];

    // Rows in [begin, end) already agree on every enclosing object, so a run
    // of rows that also agrees on this node's own columns is one element with
    // the children gathered underneath it. A node with nothing nested in it
    // has nothing to gather, and there one row stays one object -- two
    // identical rows are two elements, as they are in a plain result set.
    const bool collapse = !n.children.empty();
    std::size_t i = begin;
    bool firstElement = true;
    while (i < end) {
        std::size_t run = i + 1;
        while (collapse && run < end) {
            bool same = true;
            for (const int c : n.columns) {
                if (!(cell(r[i], c) == cell(r[run], c))) {
                    same = false;
                    break;
                }
            }
            if (!same)
                break;
            ++run;
        }

        if (!firstElement)
            o.put(',');
        firstElement = false;

        o.put('{');
        bool member = false;
        for (const int c : n.columns) {
            const Value& v = cell(r[i], c);
            if (v.isNull() && !includeNulls)
                continue;
            if (member)
                o.put(',');
            member = true;
            putKey(o, property[std::size_t(c)]);
            putValue(o, v);
        }
        for (const int child : n.children) {
            if (member)
                o.put(',');
            member = true;
            putKey(o, nodes[std::size_t(child)].name);
            o.put('[');
            emitAuto(o, child, i, run, r);
            o.put(']');
        }
        o.put('}');

        i = run;
    }
}

void JsonWriter::Impl::emit(Out& o, const Result& result) const
{
    const bool root = options.hasRoot();
    const bool wrap = !options.omitsArrayWrapper();

    if (root)
        o.put('{');
    if (root)
        putKey(o, options.rootName());
    if (wrap)
        o.put('[');

    if (options.mode() == JsonMode::Auto) {
        emitAuto(o, 0, 0, result.size(), result);
    } else {
        for (std::size_t i = 0; i < result.size(); ++i) {
            if (i)
                o.put(',');
            emitPath(o, 0, result[i]);
        }
    }

    if (wrap)
        o.put(']');
    if (root)
        o.put('}');
}

// -------------------------------------------------------------- writer ----

JsonWriter::JsonWriter() : impl_(new Impl) {}
JsonWriter::JsonWriter(JsonOptions options) : impl_(new Impl)
{
    impl_->options = std::move(options);
}
JsonWriter::~JsonWriter() = default;
JsonWriter::JsonWriter(JsonWriter&&) noexcept = default;
JsonWriter& JsonWriter::operator=(JsonWriter&&) noexcept = default;

const JsonOptions& JsonWriter::options() const noexcept
{
    return impl_->options;
}

JsonWriter& JsonWriter::options(JsonOptions options)
{
    impl_->options = std::move(options);
    return *this;
}

std::size_t JsonWriter::measure(const Result& result)
{
    return measure(result, result.columns());
}

std::size_t JsonWriter::measure(const Result& result, const std::vector<std::string>& paths)
{
    if (impl_->options.hasRoot() && impl_->options.omitsArrayWrapper())
        throw Error(ErrorCode::InvalidArgument,
                    "a JSON root and WITHOUT_ARRAY_WRAPPER cannot be combined");
    impl_->plan(paths, result.columns().size());
    Out counter;
    impl_->emit(counter, result);
    return counter.counted();
}

void JsonWriter::appendTo(const Result& result, std::string& out)
{
    appendTo(result, result.columns(), out);
}

void JsonWriter::appendTo(const Result& result, const std::vector<std::string>& paths,
                          std::string& out)
{
    // Measuring first is what keeps this to a single reallocation, and to none
    // at all once the buffer has seen a result of this size before.
    const std::size_t needed = measure(result, paths);
    if (out.capacity() < out.size() + needed)
        out.reserve(out.size() + needed);
    Out sink(out);
    impl_->emit(sink, result);
}

std::string_view JsonWriter::write(const Result& result)
{
    return write(result, result.columns());
}

std::string_view JsonWriter::write(const Result& result, const std::vector<std::string>& paths)
{
    impl_->buffer.clear();
    appendTo(result, paths, impl_->buffer);
    return impl_->buffer;
}

std::string_view JsonWriter::text() const noexcept
{
    return impl_->buffer;
}

std::size_t JsonWriter::capacity() const noexcept
{
    return impl_->buffer.capacity();
}

void JsonWriter::compact()
{
    std::string().swap(impl_->buffer);
}

// ------------------------------------------------------------ one-shot ----

void appendJson(const Result& result, std::string& out, const JsonOptions& options)
{
    JsonWriter(options).appendTo(result, out);
}

std::string toJson(const Result& result, const JsonOptions& options)
{
    std::string out;
    appendJson(result, out, options);
    return out;
}

}  // namespace sql
