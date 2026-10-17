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
#include <utility>
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
    std::int64_t integer() const
    {
        if (const auto* p = std::get_if<std::int64_t>(&v_))
            return *p;
        wrongType(Type::Integer);
    }
    double real() const
    {
        if (const auto* p = std::get_if<double>(&v_))
            return *p;
        wrongType(Type::Real);
    }
    Datetime datetime() const
    {
        if (const auto* p = std::get_if<Datetime>(&v_))
            return *p;
        wrongType(Type::Datetime);
    }
    const std::string& text() const
    {
        if (const auto* p = std::get_if<std::string>(&v_))
            return *p;
        wrongType(Type::Text);
    }
    const Blob& blob() const
    {
        if (const auto* p = std::get_if<Blob>(&v_))
            return *p;
        wrongType(Type::Blob);
    }

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

    /// SQLite's value subtype: a tag that travels with a computed value but is
    /// never stored. `'J'` marks text that is JSON, so that JSON functions and
    /// FOR JSON embed it instead of quoting it. Every setter clears it.
    std::uint8_t subtype() const noexcept { return sub_; }
    void setSubtype(std::uint8_t s) noexcept { sub_ = s; }

    // --- in-place setters: a cell that already holds text or a blob keeps
    // its buffer, so refilling a row of cells allocates nothing once the
    // buffers have grown to fit ---
    /// Exchanges two values. Two texts or two blobs trade their buffers, so
    /// neither is copied and both keep their capacity.
    void swap(Value& other) noexcept
    {
        if (auto* s = std::get_if<std::string>(&v_)) {
            if (auto* o = std::get_if<std::string>(&other.v_)) {
                s->swap(*o);
                std::swap(sub_, other.sub_);
                return;
            }
        } else if (auto* b = std::get_if<Blob>(&v_)) {
            if (auto* o = std::get_if<Blob>(&other.v_)) {
                b->swap(*o);
                std::swap(sub_, other.sub_);
                return;
            }
        }
        v_.swap(other.v_);
        std::swap(sub_, other.sub_);
    }

    void setNull() noexcept
    {
        sub_ = 0;
        if (v_.index() != 0)
            v_.emplace<std::monostate>();
    }
    void setInteger(std::int64_t i) noexcept
    {
        sub_ = 0;
        if (auto* p = std::get_if<std::int64_t>(&v_))
            *p = i;
        else
            v_.emplace<std::int64_t>(i);
    }
    void setReal(double d) noexcept
    {
        sub_ = 0;
        if (auto* p = std::get_if<double>(&v_))
            *p = d;
        else
            v_.emplace<double>(d);
    }
    void setDatetime(Datetime d) noexcept { sub_ = 0; v_.emplace<Datetime>(d); }
    void setText(std::string_view s)
    {
        sub_ = 0;
        auto* p = std::get_if<std::string>(&v_);
        if (!p)
            p = &v_.emplace<std::string>();
        p->assign(s.data(), s.size());
    }
    void setBlob(const void* data, std::size_t size)
    {
        sub_ = 0;
        const auto* p = static_cast<const std::byte*>(data);
        auto* b = std::get_if<Blob>(&v_);
        if (!b)
            b = &v_.emplace<Blob>();
        b->assign(p, p + size);
    }

    /// Reinterpret this value as `target`, the way an INSERT into a column of
    /// that type would. NULL passes through. Throws TypeMismatch.
    Value cast(Type target) const;

    /// Identity, not SQL equality: two NULLs are equal here.
    friend bool operator==(const Value& a, const Value& b) noexcept;
    friend bool operator!=(const Value& a, const Value& b) noexcept { return !(a == b); }
    friend int compare(const Value& a, const Value& b) noexcept;

private:
    /// Throws TypeMismatch for a strict accessor asked for `wanted`.
    [[noreturn]] void wrongType(Type wanted) const;

    // The alternative order must match `Type`.
    std::variant<std::monostate, std::int64_t, double, Datetime, std::string, Blob> v_;
    std::uint8_t sub_ = 0;
};

/// The subtype JSON functions put on their text results.
inline constexpr std::uint8_t kJsonSubtype = 'J';

/// Total order over values: NULL first, then numbers, datetimes, text, blobs.
/// Integers and reals compare numerically with one another; every other pair of
/// unlike classes compares by class. Returns <0, 0 or >0.
int compare(const Value& a, const Value& b) noexcept;

}  // namespace sql
