// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// SQLite's type rules: column affinity, the conversions CAST and arithmetic
// apply, the total order values sort in, collations, and LIKE/GLOB.
//
// Everything here follows SQLite 3.45 bit for bit where SQLite defines it; the
// sixth storage class, DATETIME, is libsql's and is fitted in as an instant
// that converts to its microsecond count when a number is wanted and to
// `YYYY-MM-DD HH:MM:SS[.ffffff]` when text is.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "sql/value.hpp"

namespace sql::internal {

/// Column affinity. `Blob` is SQLite's "BLOB" (formerly "NONE"): no conversion.
enum class Affinity : std::uint8_t
{
    Blob = 0,
    Text,
    Numeric,
    Integer,
    Real,
};

const char* toString(Affinity) noexcept;

/// SQLite's five rules, in order: INT -> INTEGER; CHAR, CLOB, TEXT -> TEXT;
/// BLOB or empty -> BLOB; REAL, FLOA, DOUB -> REAL; anything else NUMERIC.
Affinity affinityOfTypeName(std::string_view declared) noexcept;

/// The column types a STRICT table accepts, plus libsql's DATETIME.
enum class StrictType : std::uint8_t
{
    Any = 0,
    Integer,
    Real,
    Text,
    Blob,
    Datetime,
};

std::optional<StrictType> strictTypeOfName(std::string_view declared) noexcept;

enum class Collation : std::uint8_t
{
    Binary = 0,
    NoCase,
    RTrim,
};

std::optional<Collation> collationOfName(std::string_view name) noexcept;
const char* toString(Collation) noexcept;

// ------------------------------------------------------------- numbers ---

enum class NumKind : std::uint8_t
{
    None,
    Integer,
    Real,
};

/// Parses a decimal numeric literal the way SQLite's sqlite3AtoF does.
/// `whole` demands that nothing but whitespace surrounds it; otherwise the
/// longest numeric prefix (after leading whitespace) is used and `None` means
/// there was none. An integer literal that does not fit in 64 bits is Real.
NumKind parseNumber(std::string_view text, std::int64_t& i, double& d, bool whole) noexcept;

/// True when `d` is integral and strictly inside the int64 range, so that it
/// converts to an INTEGER without loss (sqlite3VdbeIntegerAffinity).
bool realIsExactInt(double d, std::int64_t& out) noexcept;

/// CAST(v AS INTEGER) / sqlite3VdbeIntValue: text uses its integer prefix,
/// reals truncate and saturate, NULL is 0.
std::int64_t toInt64(const Value& v) noexcept;
/// CAST(v AS REAL) / sqlite3VdbeRealValue.
double toDouble(const Value& v) noexcept;

/// The operand arithmetic sees: INTEGER and REAL pass through, text and blobs
/// use their numeric prefix (0 when there is none), a DATETIME its
/// microseconds. NULL stays NULL. `out` may alias nothing in `v`.
void numericOperand(const Value& v, Value& out);

/// SQLite truth: NULL is false (callers that need three-valued logic test
/// NULL first), numbers are true when nonzero, text and blobs by their numeric
/// prefix.
bool isTrue(const Value& v) noexcept;

// ---------------------------------------------------------------- text ---

/// SQLite's rendering of a REAL: `%!.15g` -- fifteen significant digits, and a
/// `.0` added when the result would otherwise read as an integer. "Inf",
/// "-Inf"; NaN renders as "NaN" though SQLite never stores one.
void realToText(double d, std::string& out);
std::string realToText(double d);

/// Appends the text conversion of `v` (CAST AS TEXT). NULL appends nothing.
void appendText(const Value& v, std::string& out);

/// A view of `v` as text. Text and blobs are viewed in place; anything else is
/// rendered into `scratch` and the view points there.
std::string_view textOf(const Value& v, std::string& scratch);

// --------------------------------------------------------- conversions ---

/// Converts `v` in place the way storing it in a column of affinity `a` does.
/// Never fails. DATETIME values are left alone, as are NULLs and BLOBs.
void applyAffinity(Value& v, Affinity a);

/// What a comparison applies to an operand: like applyAffinity, but under
/// TEXT affinity only numbers convert, and under numeric affinities only text
/// that is entirely a number does (SQLite's applyNumericAffinity).
void applyComparisonAffinity(Value& v, Affinity a);

/// CAST(v AS <type>), where the type name has affinity `a`. Unlike storing,
/// `Blob` affinity converts to a blob here, and a numeric CAST of text uses its
/// prefix.
void castTo(Value& v, Affinity a);

/// What a STRICT column does with a value: coerce losslessly or fail. Returns
/// false (leaving `v` alone) when the value cannot be stored.
bool strictCoerce(Value& v, StrictType t);

// ----------------------------------------------------------- ordering ----

/// Exact comparison of an integer with a real.
int compareIntReal(std::int64_t i, double r) noexcept;

int compareText(std::string_view a, std::string_view b, Collation c) noexcept;

/// The total order SQLite sorts in, with DATETIME between numbers and text:
/// NULL < INTEGER/REAL < DATETIME < TEXT < BLOB. Two NULLs compare equal.
int compareValues(const Value& a, const Value& b, Collation c = Collation::Binary) noexcept;

/// Lowercases ASCII letters only, as SQLite's NOCASE and LIKE do.
inline char foldAscii(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
}

// -------------------------------------------------------------- UTF-8 ----

/// Number of characters, as SQLite's length() counts them.
std::size_t utf8Length(std::string_view s) noexcept;
/// Reads one code point at `i`, advancing it. Malformed bytes read as
/// themselves, one byte each.
std::uint32_t utf8Next(std::string_view s, std::size_t& i) noexcept;
void appendUtf8(std::string& out, std::uint32_t cp);

// ---------------------------------------------------------- patterns -----

/// LIKE: `%` any run, `_` one character, ASCII case-insensitive unless
/// `caseSensitive`. `escape` is the escape code point, or 0 for none.
bool likeMatch(std::string_view pattern, std::string_view text, std::uint32_t escape,
               bool caseSensitive) noexcept;
/// GLOB: `*`, `?`, `[...]` with ranges and `^`, case-sensitive.
bool globMatch(std::string_view pattern, std::string_view text) noexcept;

}  // namespace sql::internal
