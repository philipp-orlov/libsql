// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "internal/func.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <string_view>

#include "internal/cnumeric.hpp"

namespace sql::internal {
namespace {

constexpr std::int64_t kMsPerSecond = 1000;
constexpr std::int64_t kMsPerMinute = 60 * kMsPerSecond;
constexpr std::int64_t kMsPerHour = 60 * kMsPerMinute;
constexpr std::int64_t kMsPerDay = 24 * kMsPerHour;
constexpr std::int64_t kUnixEpochJulianMs = 210866760000000LL;
constexpr std::int64_t kMaxJulianMs = 464269060799999LL;

struct DateTime
{
    std::int64_t iJD = 0;
    int Y = 0;
    int M = 0;
    int D = 0;
    int h = 0;
    int m = 0;
    int tz = 0;
    double s = 0.0;
    bool validJD = false;
    bool rawS = false;
    bool validYMD = false;
    bool validHMS = false;
    bool validTZ = false;
    bool tzSet = false;
    bool isError = false;
    bool useSubsec = false;
};

bool isSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

char lowerAscii(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? char(c + ('a' - 'A')) : c;
}

bool equalsNoCaseLocal(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i]))
            return false;
    }
    return true;
}

bool startsNoCase(std::string_view a, std::string_view prefix) noexcept
{
    if (prefix.size() > a.size())
        return false;
    return equalsNoCaseLocal(a.substr(0, prefix.size()), prefix);
}

std::string_view trim(std::string_view s) noexcept
{
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back()))
        s.remove_suffix(1);
    return s;
}

bool parseFixedDigits(std::string_view s, int n, int min, int max, int& out) noexcept
{
    if (s.size() < static_cast<std::size_t>(n))
        return false;
    int value = 0;
    for (int i = 0; i < n; ++i) {
        if (!isDigit(s[static_cast<std::size_t>(i)]))
            return false;
        value = value * 10 + (s[static_cast<std::size_t>(i)] - '0');
    }
    if (value < min || value > max)
        return false;
    out = value;
    return true;
}

bool parseWholeDouble(std::string_view s, double& out)
{
    s = trim(s);
    if (s.empty())
        return false;
    char buf[128];
    std::string heap;
    const char* text = nullptr;
    if (s.size() < sizeof buf) {
        std::memcpy(buf, s.data(), s.size());
        buf[s.size()] = '\0';
        text = buf;
    } else {
        heap.assign(s.data(), s.size());
        text = heap.c_str();
    }
    char* end = nullptr;
    out = parseDouble(text, &end);
    if (end == text)
        return false;
    while (*end != '\0') {
        if (!isSpace(*end))
            return false;
        ++end;
    }
    return true;
}

bool parsePrefixDouble(std::string_view s, std::size_t len, double& out)
{
    if (len > s.size())
        len = s.size();
    if (len == 0)
        return false;
    char buf[128];
    std::string heap;
    const char* text = nullptr;
    if (len < sizeof buf) {
        std::memcpy(buf, s.data(), len);
        buf[len] = '\0';
        text = buf;
    } else {
        heap.assign(s.data(), len);
        text = heap.c_str();
    }
    char* end = nullptr;
    out = parseDouble(text, &end);
    return end != text;
}

void datetimeError(DateTime& dt) noexcept
{
    dt = DateTime{};
    dt.isError = true;
}

bool validJulianDay(std::int64_t iJD) noexcept
{
    return iJD >= 0 && iJD <= kMaxJulianMs;
}

void clearYmdHmsTz(DateTime& dt) noexcept
{
    dt.validYMD = false;
    dt.validHMS = false;
    dt.validTZ = false;
}

void computeJD(DateTime& dt)
{
    if (dt.validJD)
        return;
    int year = 2000;
    int month = 1;
    int day = 1;
    if (dt.validYMD) {
        year = dt.Y;
        month = dt.M;
        day = dt.D;
    }
    if (year < -4713 || year > 9999 || dt.rawS) {
        datetimeError(dt);
        return;
    }
    if (month <= 2) {
        --year;
        month += 12;
    }
    const int a = year / 100;
    const int b = 2 - a + (a / 4);
    const int x1 = 36525 * (year + 4716) / 100;
    const int x2 = 306001 * (month + 1) / 10000;
    dt.iJD = static_cast<std::int64_t>((x1 + x2 + day + b - 1524.5) * double(kMsPerDay));
    dt.validJD = true;
    if (dt.validHMS) {
        dt.iJD += static_cast<std::int64_t>(dt.h) * kMsPerHour +
                  static_cast<std::int64_t>(dt.m) * kMsPerMinute +
                  static_cast<std::int64_t>(dt.s * 1000.0 + 0.5);
        if (dt.validTZ) {
            dt.iJD -= static_cast<std::int64_t>(dt.tz) * kMsPerMinute;
            clearYmdHmsTz(dt);
        }
    }
}

void computeYMD(DateTime& dt)
{
    if (dt.validYMD)
        return;
    if (!dt.validJD) {
        dt.Y = 2000;
        dt.M = 1;
        dt.D = 1;
    } else if (!validJulianDay(dt.iJD)) {
        datetimeError(dt);
        return;
    } else {
        const int z = static_cast<int>((dt.iJD + kMsPerDay / 2) / kMsPerDay);
        int a = static_cast<int>((z - 1867216.25) / 36524.25);
        a = z + 1 + a - (a / 4);
        const int b = a + 1524;
        const int c = static_cast<int>((b - 122.1) / 365.25);
        const int d = (36525 * (c & 32767)) / 100;
        const int e = static_cast<int>((b - d) / 30.6001);
        const int x1 = static_cast<int>(30.6001 * e);
        dt.D = b - d - x1;
        dt.M = e < 14 ? e - 1 : e - 13;
        dt.Y = dt.M > 2 ? c - 4716 : c - 4715;
    }
    dt.validYMD = true;
}

void computeHMS(DateTime& dt)
{
    if (dt.validHMS)
        return;
    computeJD(dt);
    if (dt.isError)
        return;
    int dayMs = static_cast<int>((dt.iJD + kMsPerDay / 2) % kMsPerDay);
    if (dayMs < 0)
        dayMs += static_cast<int>(kMsPerDay);
    dt.s = (dayMs % static_cast<int>(kMsPerMinute)) / 1000.0;
    const int dayMin = dayMs / static_cast<int>(kMsPerMinute);
    dt.m = dayMin % 60;
    dt.h = dayMin / 60;
    dt.rawS = false;
    dt.validHMS = true;
}

void computeYmdHms(DateTime& dt)
{
    computeYMD(dt);
    computeHMS(dt);
}

bool parseTimezone(std::string_view s, DateTime& dt)
{
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    dt.tz = 0;
    if (s.empty())
        return true;
    int sign = 0;
    if (s.front() == '-') {
        sign = -1;
    } else if (s.front() == '+') {
        sign = 1;
    } else if (s.front() == 'Z' || s.front() == 'z') {
        s.remove_prefix(1);
        while (!s.empty() && isSpace(s.front()))
            s.remove_prefix(1);
        dt.tzSet = true;
        return s.empty();
    } else {
        return false;
    }
    s.remove_prefix(1);
    int hour = 0;
    int minute = 0;
    if (s.size() < 5 || !parseFixedDigits(s, 2, 0, 14, hour) || s[2] != ':' ||
        !parseFixedDigits(s.substr(3), 2, 0, 59, minute)) {
        return false;
    }
    s.remove_prefix(5);
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    if (!s.empty())
        return false;
    dt.tz = sign * (minute + hour * 60);
    dt.tzSet = true;
    return true;
}

bool parseHhMmSs(std::string_view s, DateTime& dt)
{
    int hour = 0;
    int minute = 0;
    int second = 0;
    double ms = 0.0;
    if (s.size() < 5 || !parseFixedDigits(s, 2, 0, 24, hour) || s[2] != ':' ||
        !parseFixedDigits(s.substr(3), 2, 0, 59, minute)) {
        return false;
    }
    s.remove_prefix(5);
    if (!s.empty() && s.front() == ':') {
        s.remove_prefix(1);
        if (!parseFixedDigits(s, 2, 0, 59, second))
            return false;
        s.remove_prefix(2);
        if (!s.empty() && s.front() == '.' && s.size() >= 2 && isDigit(s[1])) {
            double scale = 1.0;
            s.remove_prefix(1);
            while (!s.empty() && isDigit(s.front())) {
                ms = ms * 10.0 + (s.front() - '0');
                scale *= 10.0;
                s.remove_prefix(1);
            }
            ms /= scale;
        }
    }
    dt.validJD = false;
    dt.rawS = false;
    dt.validHMS = true;
    dt.h = hour;
    dt.m = minute;
    dt.s = second + ms;
    if (!parseTimezone(s, dt))
        return false;
    dt.validTZ = dt.tz != 0;
    return true;
}

bool parseYyyyMmDd(std::string_view s, DateTime& dt)
{
    bool neg = false;
    if (!s.empty() && s.front() == '-') {
        neg = true;
        s.remove_prefix(1);
    }
    int year = 0;
    int month = 0;
    int day = 0;
    if (s.size() < 10 || !parseFixedDigits(s, 4, 0, 9999, year) || s[4] != '-' ||
        !parseFixedDigits(s.substr(5), 2, 1, 12, month) || s[7] != '-' ||
        !parseFixedDigits(s.substr(8), 2, 1, 31, day)) {
        return false;
    }
    s.remove_prefix(10);
    while (!s.empty() && (isSpace(s.front()) || s.front() == 'T'))
        s.remove_prefix(1);
    if (parseHhMmSs(s, dt)) {
        // parsed below
    } else if (s.empty()) {
        dt.validHMS = false;
    } else {
        return false;
    }
    dt.validJD = false;
    dt.validYMD = true;
    dt.Y = neg ? -year : year;
    dt.M = month;
    dt.D = day;
    if (dt.validTZ)
        computeJD(dt);
    return true;
}

void setRawDateNumber(DateTime& dt, double value)
{
    dt.s = value;
    dt.rawS = true;
    if (value >= 0.0 && value < 5373484.5) {
        dt.iJD = static_cast<std::int64_t>(value * double(kMsPerDay) + 0.5);
        dt.validJD = true;
    }
}

void setToNow(FuncContext& ctx, DateTime& dt)
{
    dt.iJD = kUnixEpochJulianMs + ctx.now() / 1000;
    dt.validJD = true;
}

bool parseDateOrTime(FuncContext& ctx, std::string_view input, DateTime& dt)
{
    input = trim(input);
    if (parseYyyyMmDd(input, dt))
        return true;
    if (parseHhMmSs(input, dt))
        return true;
    if (equalsNoCaseLocal(input, "now")) {
        setToNow(ctx, dt);
        return true;
    }
    if (equalsNoCaseLocal(input, "subsec") || equalsNoCaseLocal(input, "subsecond")) {
        dt.useSubsec = true;
        setToNow(ctx, dt);
        return true;
    }
    double value = 0.0;
    if (parseWholeDouble(input, value)) {
        setRawDateNumber(dt, value);
        return true;
    }
    return false;
}

void autoAdjustDate(DateTime& dt)
{
    if (!dt.rawS || dt.validJD) {
        dt.rawS = false;
    } else if (dt.s >= -21086676LL * 10000LL && dt.s <= 25340230LL * 10000LL + 799) {
        const double value = dt.s * 1000.0 + double(kUnixEpochJulianMs);
        clearYmdHmsTz(dt);
        dt.iJD = static_cast<std::int64_t>(value + 0.5);
        dt.validJD = true;
        dt.rawS = false;
    }
}

bool osLocaltime(std::time_t t, std::tm& tmOut)
{
#ifdef _WIN32
    return localtime_s(&tmOut, &t) == 0;
#else
    return localtime_r(&t, &tmOut) != nullptr;
#endif
}

bool toLocaltime(DateTime& dt)
{
    computeJD(dt);
    int yearDiff = 0;
    std::int64_t localJD = dt.iJD;
    if (dt.iJD < 2108667600LL * 100000LL || dt.iJD > 2130141456LL * 100000LL) {
        DateTime mapped = dt;
        computeYmdHms(mapped);
        yearDiff = (2000 + mapped.Y % 4) - mapped.Y;
        mapped.Y += yearDiff;
        mapped.validJD = false;
        computeJD(mapped);
        localJD = mapped.iJD;
    }
    const std::int64_t seconds = localJD / 1000 - 21086676LL * 10000LL;
    if (seconds < std::numeric_limits<std::time_t>::min() ||
        seconds > std::numeric_limits<std::time_t>::max()) {
        return false;
    }
    std::tm local {};
    if (!osLocaltime(static_cast<std::time_t>(seconds), local))
        return false;
    dt.Y = local.tm_year + 1900 - yearDiff;
    dt.M = local.tm_mon + 1;
    dt.D = local.tm_mday;
    dt.h = local.tm_hour;
    dt.m = local.tm_min;
    dt.s = static_cast<double>(local.tm_sec) + (dt.iJD % 1000) * 0.001;
    dt.validYMD = true;
    dt.validHMS = true;
    dt.validJD = false;
    dt.rawS = false;
    dt.validTZ = false;
    dt.isError = false;
    return true;
}

int weekdayOf(const DateTime& dt) noexcept
{
    return static_cast<int>(((dt.iJD + 129600000LL) / kMsPerDay) % 7);
}

bool parseHhMmSsModifier(std::string_view z, char sign, DateTime& dt)
{
    if (!z.empty() && !isDigit(z.front()))
        z.remove_prefix(1);
    DateTime tx;
    if (!parseHhMmSs(z, tx))
        return false;
    computeJD(tx);
    tx.iJD -= kMsPerDay / 2;
    const std::int64_t day = tx.iJD / kMsPerDay;
    tx.iJD -= day * kMsPerDay;
    if (sign == '-')
        tx.iJD = -tx.iJD;
    computeJD(dt);
    clearYmdHmsTz(dt);
    dt.iJD += tx.iJD;
    return true;
}

bool parseDateSpanModifier(std::string_view z, char sign, std::size_t firstLen, DateTime& dt)
{
    int year = 0;
    int month = 0;
    int day = 0;
    std::string_view digits = z.substr(1);
    const std::size_t consumed = firstLen == 5 ? 11U : 12U;
    if (firstLen == 5) {
        if (digits.size() < 10 || !parseFixedDigits(digits, 4, 0, 9999, year) ||
            digits[4] != '-' || !parseFixedDigits(digits.substr(5), 2, 0, 12, month) ||
            digits[7] != '-' || !parseFixedDigits(digits.substr(8), 2, 0, 31, day)) {
            return false;
        }
    } else {
        if (digits.size() < 11 || !parseFixedDigits(digits, 5, 0, 14712, year) ||
            digits[5] != '-' || !parseFixedDigits(digits.substr(6), 2, 0, 12, month) ||
            digits[8] != '-' || !parseFixedDigits(digits.substr(9), 2, 0, 31, day)) {
            return false;
        }
    }
    if (month >= 12 || day >= 31)
        return false;
    computeYmdHms(dt);
    dt.validJD = false;
    if (sign == '-') {
        dt.Y -= year;
        dt.M -= month;
        day = -day;
    } else {
        dt.Y += year;
        dt.M += month;
    }
    const int adjust = dt.M > 0 ? (dt.M - 1) / 12 : (dt.M - 12) / 12;
    dt.Y += adjust;
    dt.M -= adjust * 12;
    computeJD(dt);
    dt.validHMS = false;
    dt.validYMD = false;
    dt.iJD += static_cast<std::int64_t>(day) * kMsPerDay;
    if (z.size() == consumed)
        return true;
    if (z.size() > consumed + 1 && isSpace(z[consumed])) {
        int hour = 0;
        int minute = 0;
        if (parseFixedDigits(z.substr(consumed + 1), 2, 0, 24, hour) &&
            z.size() >= consumed + 6 && z[consumed + 3] == ':' &&
            parseFixedDigits(z.substr(consumed + 4), 2, 0, 59, minute)) {
            (void)hour;
            (void)minute;
            return parseHhMmSsModifier(z.substr(consumed + 1), sign, dt);
        }
    }
    return false;
}

bool parseModifier(FuncContext& ctx, std::string_view z, DateTime& dt, int index)
{
    z = trim(z);
    if (z.empty())
        return false;

    if (equalsNoCaseLocal(z, "auto")) {
        if (index > 1)
            return false;
        autoAdjustDate(dt);
        return true;
    }
    if (equalsNoCaseLocal(z, "julianday")) {
        if (index > 1)
            return false;
        if (dt.validJD && dt.rawS) {
            dt.rawS = false;
            return true;
        }
        return false;
    }
    if (equalsNoCaseLocal(z, "localtime"))
        return toLocaltime(dt);
    if (equalsNoCaseLocal(z, "unixepoch") && dt.rawS) {
        if (index > 1)
            return false;
        const double value = dt.s * 1000.0 + double(kUnixEpochJulianMs);
        if (value >= 0.0 && value < 464269060800000.0) {
            clearYmdHmsTz(dt);
            dt.iJD = static_cast<std::int64_t>(value + 0.5);
            dt.validJD = true;
            dt.rawS = false;
            return true;
        }
        return false;
    }
    if (equalsNoCaseLocal(z, "utc")) {
        if (!dt.tzSet) {
            computeJD(dt);
            std::int64_t guess = dt.iJD;
            const std::int64_t orig = dt.iJD;
            std::int64_t err = 0;
            int count = 0;
            do {
                DateTime probe;
                guess -= err;
                probe.iJD = guess;
                probe.validJD = true;
                if (!toLocaltime(probe))
                    return false;
                computeJD(probe);
                err = probe.iJD - orig;
            } while (err != 0 && count++ < 3);
            dt = DateTime{};
            dt.iJD = guess;
            dt.validJD = true;
            dt.tzSet = true;
        }
        return true;
    }
    if (startsNoCase(z, "weekday ")) {
        double value = 0.0;
        if (parseWholeDouble(z.substr(8), value) && value >= 0.0 && value < 7.0 &&
            static_cast<int>(value) == value) {
            computeYmdHms(dt);
            dt.validTZ = false;
            dt.validJD = false;
            computeJD(dt);
            int day = weekdayOf(dt);
            const int target = static_cast<int>(value);
            if (day > target)
                day -= 7;
            dt.iJD += static_cast<std::int64_t>(target - day) * kMsPerDay;
            clearYmdHmsTz(dt);
            return true;
        }
        return false;
    }
    if (equalsNoCaseLocal(z, "subsec") || equalsNoCaseLocal(z, "subsecond")) {
        dt.useSubsec = true;
        return true;
    }
    if (startsNoCase(z, "start of ")) {
        if (!dt.validJD && !dt.validYMD && !dt.validHMS)
            return false;
        std::string_view target = z.substr(9);
        computeYMD(dt);
        dt.validHMS = true;
        dt.h = 0;
        dt.m = 0;
        dt.s = 0.0;
        dt.rawS = false;
        dt.validTZ = false;
        dt.validJD = false;
        if (equalsNoCaseLocal(target, "month")) {
            dt.D = 1;
            return true;
        }
        if (equalsNoCaseLocal(target, "year")) {
            dt.M = 1;
            dt.D = 1;
            return true;
        }
        return equalsNoCaseLocal(target, "day");
    }

    const char first = z.front();
    if ((first == '+' || first == '-' || isDigit(first))) {
        std::size_t n = 1;
        int spanYear = 0;
        for (; n < z.size(); ++n) {
            if (z[n] == ':' || isSpace(z[n]))
                break;
            if (z[n] == '-') {
                if (n == 5 && parseFixedDigits(z.substr(1), 4, 0, 9999, spanYear))
                    break;
                if (n == 6 && parseFixedDigits(z.substr(1), 5, 0, 9999, spanYear))
                    break;
            }
        }
        double value = 0.0;
        if (!parsePrefixDouble(z, n, value))
            return false;
        if (n < z.size() && z[n] == '-') {
            if (first != '+' && first != '-')
                return false;
            return parseDateSpanModifier(z, first, n, dt);
        }
        if (n < z.size() && z[n] == ':')
            return parseHhMmSsModifier(z.substr(n), first, dt);

        std::string_view unit = trim(z.substr(n));
        if (!unit.empty() && lowerAscii(unit.back()) == 's')
            unit.remove_suffix(1);
        struct Transform
        {
            std::string_view name;
            double limit;
            double seconds;
        };
        static constexpr Transform transforms[] = {
            {"second", 4.6427e14, 1.0},
            {"minute", 7.7379e12, 60.0},
            {"hour", 1.2897e11, 3600.0},
            {"day", 5373485.0, 86400.0},
            {"month", 176546.0, 2592000.0},
            {"year", 14713.0, 31536000.0},
        };
        computeJD(dt);
        const double rounder = value < 0 ? -0.5 : 0.5;
        for (std::size_t i = 0; i < sizeof transforms / sizeof transforms[0]; ++i) {
            const Transform& xform = transforms[i];
            if (!equalsNoCaseLocal(unit, xform.name) || !(value > -xform.limit && value < xform.limit))
                continue;
            if (i == 4) {
                computeYmdHms(dt);
                dt.M += static_cast<int>(value);
                const int adjust = dt.M > 0 ? (dt.M - 1) / 12 : (dt.M - 12) / 12;
                dt.Y += adjust;
                dt.M -= adjust * 12;
                dt.validJD = false;
                value -= static_cast<int>(value);
            } else if (i == 5) {
                const int years = static_cast<int>(value);
                computeYmdHms(dt);
                dt.Y += years;
                dt.validJD = false;
                value -= years;
            }
            computeJD(dt);
            dt.iJD += static_cast<std::int64_t>(value * 1000.0 * xform.seconds + rounder);
            clearYmdHmsTz(dt);
            return true;
        }
    }
    (void)ctx;
    return false;
}

bool valueToDate(FuncContext& ctx, int argc, Args argv, DateTime& dt)
{
    dt = DateTime{};
    if (argc == 0) {
        setToNow(ctx, dt);
        return true;
    }
    const Value& first = *argv[0];
    if (first.isNull())
        return false;
    if (first.type() == Type::Integer || first.type() == Type::Real) {
        setRawDateNumber(dt, toDouble(first));
    } else if (first.type() == Type::Datetime) {
        // libsql extension: DATETIME stores an exact UTC Unix instant.
        dt.iJD = kUnixEpochJulianMs + first.datetime().usec / 1000;
        dt.validJD = true;
    } else {
        std::string scratch;
        if (!parseDateOrTime(ctx, textOf(first, scratch), dt))
            return false;
    }
    for (int i = 1; i < argc; ++i) {
        if (argv[i]->isNull())
            return false;
        std::string scratch;
        const std::string_view mod = textOf(*argv[i], scratch);
        if (!parseModifier(ctx, mod, dt, i))
            return false;
    }
    computeJD(dt);
    if (dt.isError || !validJulianDay(dt.iJD))
        return false;
    if (argc == 1 && dt.validYMD && dt.D > 28)
        dt.validYMD = false;
    return true;
}

void setTextResult(Value& out, const char* data, std::size_t size)
{
    out.setText(std::string_view(data, size));
}

void appendInt2(char* out, int value)
{
    out[0] = char('0' + (value / 10) % 10);
    out[1] = char('0' + value % 10);
}

void dateFunc(FuncContext& ctx, int argc, Args argv, Value& out)
{
    DateTime x;
    if (!valueToDate(ctx, argc, argv, x)) {
        out.setNull();
        return;
    }
    computeYMD(x);
    int year = x.Y < 0 ? -x.Y : x.Y;
    char buf[12];
    buf[0] = char('0' + (year / 1000) % 10);
    buf[1] = char('0' + (year / 100) % 10);
    buf[2] = char('0' + (year / 10) % 10);
    buf[3] = char('0' + year % 10);
    buf[4] = '-';
    appendInt2(buf + 5, x.M);
    buf[7] = '-';
    appendInt2(buf + 8, x.D);
    if (x.Y < 0) {
        char neg[12];
        neg[0] = '-';
        std::memcpy(neg + 1, buf, 10);
        setTextResult(out, neg, 11);
    } else {
        setTextResult(out, buf, 10);
    }
}

void timeFunc(FuncContext& ctx, int argc, Args argv, Value& out)
{
    DateTime x;
    if (!valueToDate(ctx, argc, argv, x)) {
        out.setNull();
        return;
    }
    computeHMS(x);
    char buf[16];
    appendInt2(buf, x.h);
    buf[2] = ':';
    appendInt2(buf + 3, x.m);
    buf[5] = ':';
    if (x.useSubsec) {
        const int s = static_cast<int>(1000.0 * x.s + 0.5);
        buf[6] = char('0' + (s / 10000) % 10);
        buf[7] = char('0' + (s / 1000) % 10);
        buf[8] = '.';
        buf[9] = char('0' + (s / 100) % 10);
        buf[10] = char('0' + (s / 10) % 10);
        buf[11] = char('0' + s % 10);
        setTextResult(out, buf, 12);
    } else {
        const int s = static_cast<int>(x.s);
        appendInt2(buf + 6, s);
        setTextResult(out, buf, 8);
    }
}

void datetimeFunc(FuncContext& ctx, int argc, Args argv, Value& out)
{
    DateTime x;
    if (!valueToDate(ctx, argc, argv, x)) {
        out.setNull();
        return;
    }
    computeYmdHms(x);
    int year = x.Y < 0 ? -x.Y : x.Y;
    char buf[32];
    int n = std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:", year, x.M, x.D, x.h, x.m);
    if (x.useSubsec) {
        const int s = static_cast<int>(1000.0 * x.s + 0.5);
        n += std::snprintf(buf + n, sizeof buf - static_cast<std::size_t>(n), "%02d.%03d", s / 1000,
                           s % 1000);
    } else {
        n += std::snprintf(buf + n, sizeof buf - static_cast<std::size_t>(n), "%02d",
                           static_cast<int>(x.s));
    }
    if (x.Y < 0) {
        char neg[32];
        neg[0] = '-';
        std::memcpy(neg + 1, buf, static_cast<std::size_t>(n));
        setTextResult(out, neg, static_cast<std::size_t>(n + 1));
    } else {
        setTextResult(out, buf, static_cast<std::size_t>(n));
    }
}

void juliandayFunc(FuncContext& ctx, int argc, Args argv, Value& out)
{
    DateTime x;
    if (!valueToDate(ctx, argc, argv, x)) {
        out.setNull();
        return;
    }
    computeJD(x);
    out.setReal(x.iJD / double(kMsPerDay));
}

void unixepochFunc(FuncContext& ctx, int argc, Args argv, Value& out)
{
    DateTime x;
    if (!valueToDate(ctx, argc, argv, x)) {
        out.setNull();
        return;
    }
    computeJD(x);
    if (x.useSubsec) {
        out.setReal((x.iJD - kUnixEpochJulianMs) / 1000.0);
    } else {
        out.setInteger(x.iJD / 1000 - 21086676LL * 10000LL);
    }
}

void appendFormat(std::string& out, const char* fmt, int value)
{
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, fmt, value);
    out.append(buf, static_cast<std::size_t>(n));
}

void appendFormat64(std::string& out, const char* fmt, long long value)
{
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, fmt, value);
    out.append(buf, static_cast<std::size_t>(n));
}

void strftimeFunc(FuncContext& ctx, int argc, Args argv, Value& out)
{
    if (argc == 0 || argv[0]->isNull()) {
        out.setNull();
        return;
    }
    std::string fmtScratch;
    const std::string_view format = textOf(*argv[0], fmtScratch);
    DateTime x;
    if (!valueToDate(ctx, argc - 1, argv + 1, x)) {
        out.setNull();
        return;
    }
    computeJD(x);
    computeYmdHms(x);
    std::string result;
    result.reserve(format.size() + 32);
    std::size_t last = 0;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%')
            continue;
        result.append(format.substr(last, i - last));
        ++i;
        if (i >= format.size()) {
            out.setNull();
            return;
        }
        last = i + 1;
        switch (format[i]) {
            case 'd': appendFormat(result, "%02d", x.D); break;
            case 'e': appendFormat(result, "%2d", x.D); break;
            case 'f': {
                double seconds = x.s;
                if (seconds > 59.999)
                    seconds = 59.999;
                char buf[16];
                const int n = std::snprintf(buf, sizeof buf, "%06.3f", seconds);
                result.append(buf, static_cast<std::size_t>(n));
                break;
            }
            case 'F': {
                appendFormat(result, "%04d", x.Y);
                result += '-';
                appendFormat(result, "%02d", x.M);
                result += '-';
                appendFormat(result, "%02d", x.D);
                break;
            }
            case 'H': appendFormat(result, "%02d", x.h); break;
            case 'k': appendFormat(result, "%2d", x.h); break;
            case 'I':
            case 'l': {
                int hour = x.h;
                if (hour > 12)
                    hour -= 12;
                if (hour == 0)
                    hour = 12;
                appendFormat(result, format[i] == 'I' ? "%02d" : "%2d", hour);
                break;
            }
            case 'j':
            case 'W': {
                DateTime y = x;
                y.validJD = false;
                y.M = 1;
                y.D = 1;
                computeJD(y);
                const int day = static_cast<int>((x.iJD - y.iJD + kMsPerDay / 2) / kMsPerDay);
                if (format[i] == 'W') {
                    const int wd = static_cast<int>(((x.iJD + kMsPerDay / 2) / kMsPerDay) % 7);
                    appendFormat(result, "%02d", (day + 7 - wd) / 7);
                } else {
                    appendFormat(result, "%03d", day + 1);
                }
                break;
            }
            case 'J': {
                char buf[64];
                const int n = formatDouble(buf, sizeof buf, "%.16g", x.iJD / double(kMsPerDay));
                result.append(buf, static_cast<std::size_t>(n));
                break;
            }
            case 'm': appendFormat(result, "%02d", x.M); break;
            case 'M': appendFormat(result, "%02d", x.m); break;
            case 'p': result += x.h >= 12 ? "PM" : "AM"; break;
            case 'P': result += x.h >= 12 ? "pm" : "am"; break;
            case 'R': {
                appendFormat(result, "%02d", x.h);
                result += ':';
                appendFormat(result, "%02d", x.m);
                break;
            }
            case 's':
                if (x.useSubsec) {
                    char buf[64];
                    const int n = formatDouble(buf, sizeof buf, "%.3f", (x.iJD - kUnixEpochJulianMs) / 1000.0);
                    result.append(buf, static_cast<std::size_t>(n));
                } else {
                    appendFormat64(result, "%lld", static_cast<long long>(x.iJD / 1000 - 21086676LL * 10000LL));
                }
                break;
            case 'S': appendFormat(result, "%02d", static_cast<int>(x.s)); break;
            case 'T': {
                appendFormat(result, "%02d", x.h);
                result += ':';
                appendFormat(result, "%02d", x.m);
                result += ':';
                appendFormat(result, "%02d", static_cast<int>(x.s));
                break;
            }
            case 'u':
            case 'w': {
                char c = char(weekdayOf(x) + '0');
                if (c == '0' && format[i] == 'u')
                    c = '7';
                result += c;
                break;
            }
            case 'Y': appendFormat(result, "%04d", x.Y); break;
            case '%': result += '%'; break;
            default:
                out.setNull();
                return;
        }
    }
    result.append(format.substr(last));
    out.setText(result);
}

void timediffFunc(FuncContext& ctx, int argc, Args argv, Value& out)
{
    (void)argc;
    DateTime d1;
    DateTime d2;
    if (!valueToDate(ctx, 1, argv, d1) || !valueToDate(ctx, 1, argv + 1, d2)) {
        out.setNull();
        return;
    }
    computeYmdHms(d1);
    computeYmdHms(d2);
    char sign = '+';
    int years = 0;
    int months = 0;
    if (d1.iJD >= d2.iJD) {
        years = d1.Y - d2.Y;
        if (years != 0) {
            d2.Y = d1.Y;
            d2.validJD = false;
            computeJD(d2);
        }
        months = d1.M - d2.M;
        if (months < 0) {
            --years;
            months += 12;
        }
        if (months != 0) {
            d2.M = d1.M;
            d2.validJD = false;
            computeJD(d2);
        }
        while (d1.iJD < d2.iJD) {
            --months;
            if (months < 0) {
                months = 11;
                --years;
            }
            --d2.M;
            if (d2.M < 1) {
                d2.M = 12;
                --d2.Y;
            }
            d2.validJD = false;
            computeJD(d2);
        }
        d1.iJD -= d2.iJD;
        d1.iJD += 1486995408LL * 100000LL;
    } else {
        sign = '-';
        years = d2.Y - d1.Y;
        if (years != 0) {
            d2.Y = d1.Y;
            d2.validJD = false;
            computeJD(d2);
        }
        months = d2.M - d1.M;
        if (months < 0) {
            --years;
            months += 12;
        }
        if (months != 0) {
            d2.M = d1.M;
            d2.validJD = false;
            computeJD(d2);
        }
        while (d1.iJD > d2.iJD) {
            --months;
            if (months < 0) {
                months = 11;
                --years;
            }
            ++d2.M;
            if (d2.M > 12) {
                d2.M = 1;
                ++d2.Y;
            }
            d2.validJD = false;
            computeJD(d2);
        }
        d1.iJD = d2.iJD - d1.iJD;
        d1.iJD += 1486995408LL * 100000LL;
    }
    d1.validYMD = false;
    d1.validHMS = false;
    d1.validTZ = false;
    computeYmdHms(d1);
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "%c%04d-%02d-%02d %02d:%02d:%06.3f", sign, years,
                                months, d1.D - 1, d1.h, d1.m, d1.s);
    out.setText(std::string_view(buf, static_cast<std::size_t>(n)));
}

void currentDateFunc(FuncContext& ctx, int argc, Args argv, Value& out)
{
    dateFunc(ctx, argc, argv, out);
}

void currentTimeFunc(FuncContext& ctx, int argc, Args argv, Value& out)
{
    timeFunc(ctx, argc, argv, out);
}

void currentTimestampFunc(FuncContext& ctx, int argc, Args argv, Value& out)
{
    datetimeFunc(ctx, argc, argv, out);
}

}  // namespace

void registerDateFunctions(FunctionRegistry& r)
{
    r.add(FunctionDef{"julianday", 0, -1, 0, juliandayFunc, nullptr});
    r.add(FunctionDef{"unixepoch", 0, -1, 0, unixepochFunc, nullptr});
    r.add(FunctionDef{"date", 0, -1, 0, dateFunc, nullptr});
    r.add(FunctionDef{"time", 0, -1, 0, timeFunc, nullptr});
    r.add(FunctionDef{"datetime", 0, -1, 0, datetimeFunc, nullptr});
    r.add(FunctionDef{"strftime", 1, -1, 0, strftimeFunc, nullptr});
    r.add(FunctionDef{"timediff", 2, 2, 0, timediffFunc, nullptr});
    r.add(FunctionDef{"current_date", 0, 0, 0, currentDateFunc, nullptr});
    r.add(FunctionDef{"current_time", 0, 0, 0, currentTimeFunc, nullptr});
    r.add(FunctionDef{"current_timestamp", 0, 0, 0, currentTimestampFunc, nullptr});
}

}  // namespace sql::internal
