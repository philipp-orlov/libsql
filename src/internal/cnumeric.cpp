// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "internal/cnumeric.hpp"

#include <clocale>
#include <cstdio>
#include <initializer_list>
#include <cstdlib>

#ifdef _WIN32
#include <locale.h>
#else
#include <locale.h>
#endif

namespace sql::internal {
namespace {

#ifdef _WIN32
_locale_t cLocale() noexcept
{
    static const _locale_t locale = _create_locale(LC_ALL, "C");
    return locale;
}
#else
locale_t cLocale() noexcept
{
    static const locale_t locale = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
    return locale;
}

/// Makes the C locale current for this thread only, so snprintf writes '.'.
class ScopedCLocale
{
public:
    ScopedCLocale() noexcept : previous_(uselocale(cLocale())) {}
    ~ScopedCLocale() { uselocale(previous_); }
    ScopedCLocale(const ScopedCLocale&) = delete;
    ScopedCLocale& operator=(const ScopedCLocale&) = delete;

private:
    locale_t previous_;
};
#endif

}  // namespace

double parseDouble(const char* text, char** end) noexcept
{
#ifdef _WIN32
    return _strtod_l(text, end, cLocale());
#else
    return strtod_l(text, end, cLocale());
#endif
}

int formatShortest(char* buf, std::size_t size, double v) noexcept
{
#ifndef _WIN32
    const ScopedCLocale scope;
#endif
    int n = 0;
    for (const int precision : {15, 16, 17}) {
#ifdef _WIN32
        n = _snprintf_l(buf, size, "%.*g", cLocale(), precision, v);
#else
        n = std::snprintf(buf, size, "%.*g", precision, v);
#endif
        if (parseDouble(buf, nullptr) == v)
            break;
    }
    return n;
}

}  // namespace sql::internal
