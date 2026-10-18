// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libsql -- the six storage classes and the dynamically typed cell that holds
// one of them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
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

/// The bytes of a BLOB, viewed: what `std::string_view` is to TEXT.
class BlobView
{
public:
    constexpr BlobView() noexcept = default;
    constexpr BlobView(const std::byte* data, std::size_t size) noexcept : data_(data), size_(size)
    {}
    BlobView(const Blob& b) noexcept : data_(b.data()), size_(b.size()) {}

    const std::byte* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    const std::byte* begin() const noexcept { return data_; }
    const std::byte* end() const noexcept { return data_ + size_; }
    std::byte operator[](std::size_t i) const noexcept { return data_[i]; }
    /// An owned copy.
    Blob toBlob() const { return Blob(begin(), end()); }

    friend bool operator==(BlobView a, BlobView b) noexcept
    {
        return a.size_ == b.size_ && (a.size_ == 0 || std::memcmp(a.data_, b.data_, a.size_) == 0);
    }
    friend bool operator!=(BlobView a, BlobView b) noexcept { return !(a == b); }

private:
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

/// One cell. Default-constructed cells are NULL.
///
/// A TEXT or BLOB cell either owns its bytes or borrows them. Rows handed to
/// Statement::each and arguments handed to functions may borrow bytes that
/// live in the database file: such a cell is valid for as long as the row or
/// the call. Copying a cell always yields one that owns its bytes; moving or
/// swapping carries a borrow along.
class Value
{
public:
    Value() noexcept { u_.i = 0; }
    Value(std::nullptr_t) noexcept : Value() {}
    /// Every integral type, `bool` included, lands in one INTEGER cell.
    template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    Value(T i) noexcept : type_(Type::Integer)
    {
        u_.i = std::int64_t(i);
    }
    Value(double d) noexcept : type_(Type::Real) { u_.r = d; }
    Value(float d) noexcept : Value(double(d)) {}
    Value(Datetime d) noexcept : type_(Type::Datetime) { u_.i = d.usec; }
    Value(std::string_view s) { initBytes(Type::Text, s.data(), s.size()); }
    Value(const std::string& s) : Value(std::string_view(s)) {}
    Value(const char* s) : Value(std::string_view(s ? s : "")) {}
    Value(BlobView b) { initBytes(Type::Blob, reinterpret_cast<const char*>(b.data()), b.size()); }
    Value(const Blob& b) : Value(BlobView(b)) {}

    Value(const Value& other) { copyFrom(other); }
    Value(Value&& other) noexcept
        : u_(other.u_),
          type_(other.type_),
          store_(other.store_),
          sub_(other.sub_),
          size_(other.size_)
    {
        other.type_ = Type::Null;
        other.store_ = kInline;
    }
    Value& operator=(const Value& other)
    {
        if (this != &other)
            assign(other);
        return *this;
    }
    Value& operator=(Value&& other) noexcept
    {
        if (this != &other) {
            release();
            u_ = other.u_;
            type_ = other.type_;
            store_ = other.store_;
            sub_ = other.sub_;
            size_ = other.size_;
            other.type_ = Type::Null;
            other.store_ = kInline;
        }
        return *this;
    }
    ~Value() { release(); }

    static Value null() noexcept { return Value(); }
    static Value text(std::string_view s) { return Value(s); }
    static Value blob(const void* data, std::size_t size)
    {
        return Value(BlobView(static_cast<const std::byte*>(data), size));
    }
    /// Cells that borrow: `s` (or the bytes) must outlive the cell.
    static Value textRef(std::string_view s) noexcept
    {
        Value v;
        v.setTextRef(s);
        return v;
    }
    static Value blobRef(const void* data, std::size_t size) noexcept
    {
        Value v;
        v.setBlobRef(data, size);
        return v;
    }

    Type type() const noexcept { return type_; }
    bool isNull() const noexcept { return type_ == Type::Null; }
    bool isNumeric() const noexcept { return type_ == Type::Integer || type_ == Type::Real; }
    /// True for TEXT or BLOB whose bytes this cell does not own.
    bool borrowed() const noexcept { return store_ == kRef; }
    /// Makes a borrowing cell own a copy of its bytes; otherwise does nothing.
    void own()
    {
        if (store_ == kRef) {
            Value owned;
            owned.setBytes(type_, u_.ext.data, u_.ext.size);
            owned.sub_ = sub_;
            swap(owned);
        }
    }

    // --- strict accessors: the stored class must match ---
    std::int64_t integer() const
    {
        if (type_ == Type::Integer)
            return u_.i;
        wrongType(Type::Integer);
    }
    double real() const
    {
        if (type_ == Type::Real)
            return u_.r;
        wrongType(Type::Real);
    }
    Datetime datetime() const
    {
        if (type_ == Type::Datetime)
            return Datetime{u_.i};
        wrongType(Type::Datetime);
    }
    std::string_view text() const
    {
        if (type_ == Type::Text)
            return bytes();
        wrongType(Type::Text);
    }
    BlobView blob() const
    {
        if (type_ == Type::Blob) {
            const std::string_view b = bytes();
            return BlobView(reinterpret_cast<const std::byte*>(b.data()), b.size());
        }
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

    // --- in-place setters: a cell that already owns a buffer for text or a
    // blob keeps it, so refilling a row of cells allocates nothing once the
    // buffers have grown to fit ---
    /// Exchanges two values, borrows included; nothing is copied.
    void swap(Value& other) noexcept
    {
        std::swap(u_, other.u_);
        std::swap(type_, other.type_);
        std::swap(store_, other.store_);
        std::swap(sub_, other.sub_);
        std::swap(size_, other.size_);
    }

    void setNull() noexcept
    {
        release();
        sub_ = 0;
        type_ = Type::Null;
    }
    void setInteger(std::int64_t i) noexcept
    {
        release();
        sub_ = 0;
        type_ = Type::Integer;
        u_.i = i;
    }
    void setReal(double d) noexcept
    {
        release();
        sub_ = 0;
        type_ = Type::Real;
        u_.r = d;
    }
    void setDatetime(Datetime d) noexcept
    {
        release();
        sub_ = 0;
        type_ = Type::Datetime;
        u_.i = d.usec;
    }
    void setText(std::string_view s) { setBytes(Type::Text, s.data(), s.size()); }
    void setBlob(const void* data, std::size_t size)
    {
        setBytes(Type::Blob, static_cast<const char*>(data), size);
    }
    /// Borrowing setters: the bytes must outlive the cell's use of them.
    void setTextRef(std::string_view s) noexcept { setRef(Type::Text, s.data(), s.size()); }
    void setBlobRef(const void* data, std::size_t size) noexcept
    {
        setRef(Type::Blob, static_cast<const char*>(data), size);
    }
    /// Becomes `other` without copying text or blob bytes: this cell borrows
    /// them, whether `other` owns them or borrows them itself.
    void borrow(const Value& other) noexcept
    {
        if (&other == this)
            return;
        if (other.type_ == Type::Text || other.type_ == Type::Blob) {
            const std::string_view b = other.bytes();
            setRef(other.type_, b.data(), b.size());
        } else {
            release();
            u_ = other.u_;
            type_ = other.type_;
        }
        sub_ = other.sub_;
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

    /// Where TEXT and BLOB bytes are: inside the cell, in a buffer it owns,
    /// or borrowed. Other classes leave it at kInline.
    enum Store : std::uint8_t
    {
        kInline,
        kHeap,
        kRef,
    };
    static constexpr std::size_t kInlineBytes = 24;

    std::string_view bytes() const noexcept
    {
        return store_ == kInline ? std::string_view(u_.inl, size_)
                                 : std::string_view(u_.ext.data, u_.ext.size);
    }
    void release() noexcept
    {
        if (store_ != kInline) {
            if (store_ == kHeap)
                freeBytes(u_.ext.data);
            store_ = kInline;
        }
    }
    static void freeBytes(char* p) noexcept;
    void setRef(Type t, const char* p, std::size_t n) noexcept
    {
        release();
        sub_ = 0;
        type_ = t;
        store_ = kRef;
        u_.ext.data = const_cast<char*>(p);
        u_.ext.size = n;
    }
    void initBytes(Type t, const char* p, std::size_t n)
    {
        u_.i = 0;
        setBytes(t, p, n);
    }
    void setBytes(Type t, const char* p, std::size_t n);
    void copyFrom(const Value& other);
    void assign(const Value& other);

    union Storage
    {
        std::int64_t i;
        double r;
        struct
        {
            char* data;
            std::size_t size;
            std::size_t capacity;  ///< kHeap only
        } ext;
        char inl[kInlineBytes];
    } u_;
    Type type_ = Type::Null;
    std::uint8_t store_ = kInline;
    std::uint8_t sub_ = 0;
    std::uint8_t size_ = 0;  ///< bytes held inline
};

/// The subtype JSON functions put on their text results.
inline constexpr std::uint8_t kJsonSubtype = 'J';

/// Total order over values: NULL first, then numbers, datetimes, text, blobs.
/// Integers and reals compare numerically with one another; every other pair of
/// unlike classes compares by class. Returns <0, 0 or >0.
int compare(const Value& a, const Value& b) noexcept;

}  // namespace sql
