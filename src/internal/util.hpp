// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Small ASCII helpers used everywhere identifiers are compared. ASCII only on
// purpose: SQLite folds identifier case for ASCII letters only, too.
#pragma once

#include <string>
#include <string_view>

namespace sql::internal {

inline char lowerAscii(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
}

inline char upperAscii(char c) noexcept
{
    return (c >= 'a' && c <= 'z') ? char(c - 32) : c;
}

inline bool equalsNoCase(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i]))
            return false;
    }
    return true;
}

inline std::string toLower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = lowerAscii(c);
    return out;
}

inline std::string toUpper(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = upperAscii(c);
    return out;
}

}  // namespace sql::internal
