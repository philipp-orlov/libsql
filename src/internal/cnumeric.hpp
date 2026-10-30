// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Number <-> text conversion that does not depend on the process's locale.
// A host that calls setlocale(LC_NUMERIC, "de_DE") would otherwise make
// strtod read "1.5" as 1 and snprintf write "1,5", which SQL text and JSON
// must never contain.
#pragma once

#include <cstddef>

namespace sql::internal {

/// strtod in the C locale.
double parseDouble(const char* text, char** end) noexcept;

/// The shortest of 15, 16 and 17 significant digits that reads back as `v`,
/// in the C locale ("1.5", never "1,5"). Returns the length written.
int formatShortest(char* buf, std::size_t size, double v) noexcept;

/// `%.<precision>g` in the C locale. Returns the length written.
int formatPrecision(char* buf, std::size_t size, double v, int precision) noexcept;

/// printf-style formatting of one double in the C locale: `spec` is a complete
/// conversion such as "%.3f" or "%+08.2e". Returns the length written.
int formatDouble(char* buf, std::size_t size, const char* spec, double v) noexcept;

}  // namespace sql::internal
