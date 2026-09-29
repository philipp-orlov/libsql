// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libsql -- the six storage classes and the dynamically typed cell that holds
// one of them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "sql/error.hpp"

namespace sql {

/// Storage classes, in the order values of different classes sort in.
enum class Type : std::uint8_t
{
    Null = 0,
    Integer,   ///< signed 64-bit
    Real,      ///< IEEE-754 double
    Datetime,  ///< microseconds since 1970-01-01T00:00:00Z
    Text,      ///< UTF-8 (or any) bytes, compared byte-wise
    Blob,      ///< opaque bytes
};

const char* toString(Type) noexcept;
/// Parses a declared column type: INT/INTEGER, REAL/FLOAT/DOUBLE, TEXT/VARCHAR/
/// CHAR/STRING, DATETIME/TIMESTAMP/DATE, BLOB. Case-insensitive.
std::optional<Type> typeFromName(std::string_view name) noexcept;

using Blob = std::vector<std::byte>;

/// A point in time, held as microseconds since the Unix epoch in UTC so that
/// the numeric order and the calendar order are the same thing.
struct Datetime
{
    std::int64_t usec = 0;

    friend bool operator==(Datetime a, Datetime b) noexcept { return a.usec == b.usec; }
    friend bool operator!=(Datetime a, Datetime b) noexcept { return a.usec != b.usec; }
    friend bool operator<(Datetime a, Datetime b) noexcept { return a.usec < b.usec; }
};

/// Accepts `YYYY-MM-DD`, `YYYY-MM-DD HH:MM[:SS[.ffffff]]` with `T` allowed in
/// place of the space and an optional trailing `Z`. Throws TypeMismatch.
Datetime parseDatetime(std::string_view text);
/// `YYYY-MM-DD HH:MM:SS`, with `.ffffff` appended when the fraction is nonzero.
std::string formatDatetime(Datetime when);
/// The same instant as ISO-8601 `YYYY-MM-DDTHH:MM:SS[.ffffff]Z`, written into
/// `buf` without allocating. Returns the number of bytes written, which is
/// never more than 32; `size` must be at least that. Throws InvalidArgument
/// for a smaller buffer.
std::size_t formatDatetimeIso(Datetime when, char* buf, std::size_t size);

/// One cell. Default-constructed cells are NULL.
class Value
{
public:
    Value() noexcept = default;
    Value(std::nullptr_t) noexcept {}
    /// Every integral type, `bool` included, lands in one INTEGER cell.
    template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    Value(T i) noexcept : v_(std::int64_t(i))
    {}
    Value(double d) noexcept : v_(d) {}
    Value(float d) noexcept : v_(double(d)) {}
    Value(Datetime d) noexcept : v_(d) {}
    Value(std::string s) noexcept : v_(std::move(s)) {}
    Value(std::string_view s) : v_(std::string(s)) {}
    Value(const char* s) : v_(std::string(s ? s : "")) {}
    Value(Blob b) noexcept : v_(std::move(b)) {}

    static Value null() noexcept { return Value(); }
    static Value text(std::string s) noexcept { return Value(std::move(s)); }
    static Value blob(const void* data, std::size_t size);

    Type type() const noexcept { return Type(v_.index()); }
    bool isNull() const noexcept { return v_.index() == 0; }
    bool isNumeric() const noexcept { return type() == Type::Integer || type() == Type::Real; }

    // --- strict accessors: the stored class must match ---
    std::int64_t integer() const;
    double real() const;
    Datetime datetime() const;
    const std::string& text() const;
    const Blob& blob() const;

    // --- lenient accessors, for callers that just want a number or a string ---
    /// Reals truncate, text and datetimes convert; NULL and blobs throw.
    std::int64_t toInteger() const;
    double toReal() const;
    /// Display form. NULL renders as the empty string, blobs as lowercase hex.
    std::string toText() const;
    /// SQL literal form: NULL, 42, 1.5, 'text', 2024-01-02 03:04:05, x'00ff'.
    std::string toLiteral() const;

    /// SQL truth: NULL and zero are false, everything else is true.
    bool truthy() const noexcept;

    // --- in-place setters: a cell that already holds text or a blob keeps
    // its buffer, so refilling a row of cells allocates nothing once the
    // buffers have grown to fit ---
    void setNull() noexcept { v_.emplace<std::monostate>(); }
    void setInteger(std::int64_t i) noexcept { v_.emplace<std::int64_t>(i); }
    void setReal(double d) noexcept { v_.emplace<double>(d); }
    void setDatetime(Datetime d) noexcept { v_.emplace<Datetime>(d); }
    void setText(std::string_view s)
    {
        if (auto* p = std::get_if<std::string>(&v_))
            p->assign(s.data(), s.size());
        else
            v_.emplace<std::string>(s);
    }
    void setBlob(const void* data, std::size_t size)
    {
        const auto* p = static_cast<const std::byte*>(data);
        if (auto* b = std::get_if<Blob>(&v_))
            b->assign(p, p + size);
        else
            v_.emplace<Blob>(p, p + size);
    }

    /// Reinterpret this value as `target`, the way an INSERT into a column of
    /// that type would. NULL passes through. Throws TypeMismatch.
    Value cast(Type target) const;

    /// Identity, not SQL equality: two NULLs are equal here.
    friend bool operator==(const Value& a, const Value& b) noexcept;
    friend bool operator!=(const Value& a, const Value& b) noexcept { return !(a == b); }
    friend int compare(const Value& a, const Value& b) noexcept;

private:
    // The alternative order must match `Type`.
    std::variant<std::monostate, std::int64_t, double, Datetime, std::string, Blob> v_;
};

/// Total order over values: NULL first, then numbers, datetimes, text, blobs.
/// Integers and reals compare numerically with one another; every other pair of
/// unlike classes compares by class. Returns <0, 0 or >0.
int compare(const Value& a, const Value& b) noexcept;

}  // namespace sql
