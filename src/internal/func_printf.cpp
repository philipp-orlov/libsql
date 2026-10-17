// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "internal/func.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "internal/types.hpp"

namespace sql::internal {
namespace {

struct FormatArg
{
    int argc = 0;
    Args argv = nullptr;
    int used = 0;
    std::string* scratch = nullptr;
};

std::int64_t getIntArg(FormatArg& args) noexcept
{
    if (args.used >= args.argc)
        return 0;
    return toInt64(*args.argv[args.used++]);
}

double getDoubleArg(FormatArg& args) noexcept
{
    if (args.used >= args.argc)
        return 0.0;
    return toDouble(*args.argv[args.used++]);
}

std::string_view getTextArg(FormatArg& args)
{
    if (args.used >= args.argc)
        return std::string_view();
    args.scratch->clear();
    // SQLite formats the argument as a C string: it ends at the first NUL.
    std::string_view s = textOf(*args.argv[args.used++], *args.scratch);
    const std::size_t z = s.find('\0');
    return z == std::string_view::npos ? s : s.substr(0, z);
}

bool utf8Cont(unsigned char c) noexcept
{
    return (c & 0xC0U) == 0x80U;
}

std::size_t firstUtf8CharBytes(std::string_view s) noexcept
{
    if (s.empty())
        return 0;
    const unsigned char c = static_cast<unsigned char>(s[0]);
    std::size_t n = 1;
    if ((c & 0xE0U) == 0xC0U)
        n = 2;
    else if ((c & 0xF0U) == 0xE0U)
        n = 3;
    else if ((c & 0xF8U) == 0xF0U)
        n = 4;
    n = std::min(n, s.size());
    for (std::size_t i = 1; i < n; ++i) {
        if (!utf8Cont(static_cast<unsigned char>(s[i])))
            return i;
    }
    return n;
}

std::size_t bytesForUtf8Chars(std::string_view s, int chars) noexcept
{
    if (chars < 0)
        return s.size();
    std::size_t i = 0;
    while (chars-- > 0 && i < s.size())
        i += firstUtf8CharBytes(s.substr(i));
    return i;
}

int extraUtf8ContinuationBytes(std::string_view s) noexcept
{
    int extra = 0;
    for (unsigned char c : s) {
        if (utf8Cont(c))
            ++extra;
    }
    return extra;
}

void appendPadded(std::string_view text, int width, bool left, bool altUtf8Width, std::string& out)
{
    int length = static_cast<int>(text.size());
    if (altUtf8Width)
        width += extraUtf8ContinuationBytes(text);
    int pad = width - length;
    if (pad > 0 && !left)
        out.append(static_cast<std::size_t>(pad), ' ');
    out.append(text);
    if (pad > 0 && left)
        out.append(static_cast<std::size_t>(pad), ' ');
}

struct FpDecode
{
    char sign = '+';
    char isSpecial = 0;  // 1: infinity, 2: NaN
    int n = 0;
    int iDP = 0;
    const char* z = nullptr;
    char zBuf[32] {};
};

void fpDecode(FpDecode& p, double input, int iRound, int mxRound)
{
    p = FpDecode{};
    p.z = p.zBuf;
    double r = input;
    if (r < 0.0) {
        p.sign = '-';
        r = -r;
    } else if (r == 0.0) {
        p.sign = '+';
        p.n = 1;
        p.iDP = 1;
        p.z = "0";
        return;
    } else {
        p.sign = '+';
    }
    std::uint64_t bits = 0;
    static_assert(sizeof bits == sizeof r, "double size");
    std::memcpy(&bits, &r, sizeof bits);
    const int expBits = static_cast<int>(bits >> 52);
    if ((expBits & 0x7ff) == 0x7ff) {
        p.isSpecial = static_cast<char>(1 + (bits != 0x7ff0000000000000ULL));
        return;
    }

    long double rr = r;
    int exp = 0;
    if (rr >= 1.0e+19L) {
        while (rr >= 1.0e+119L) {
            exp += 100;
            rr *= 1.0e-100L;
        }
        while (rr >= 1.0e+29L) {
            exp += 10;
            rr *= 1.0e-10L;
        }
        while (rr >= 1.0e+19L) {
            ++exp;
            rr *= 1.0e-1L;
        }
    } else {
        while (rr < 1.0e-97L) {
            exp -= 100;
            rr *= 1.0e+100L;
        }
        while (rr < 1.0e+07L) {
            exp -= 10;
            rr *= 1.0e+10L;
        }
        while (rr < 1.0e+17L) {
            --exp;
            rr *= 1.0e+1L;
        }
    }
    std::uint64_t v = static_cast<std::uint64_t>(rr);
    int i = static_cast<int>(sizeof p.zBuf) - 1;
    while (v != 0) {
        p.zBuf[static_cast<std::size_t>(i--)] = char('0' + (v % 10));
        v /= 10;
    }
    p.n = static_cast<int>(sizeof p.zBuf) - 1 - i;
    p.iDP = p.n + exp;
    if (iRound < 0) {
        iRound = p.iDP - iRound;
        if (iRound == 0 && p.zBuf[static_cast<std::size_t>(i + 1)] >= '5') {
            iRound = 1;
            p.zBuf[static_cast<std::size_t>(i--)] = '0';
            ++p.n;
            ++p.iDP;
        }
    }
    if (iRound > 0 && (iRound < p.n || p.n > mxRound)) {
        char* z = &p.zBuf[static_cast<std::size_t>(i + 1)];
        if (iRound > mxRound)
            iRound = mxRound;
        p.n = iRound;
        if (z[iRound] >= '5') {
            int j = iRound - 1;
            for (;;) {
                ++z[j];
                if (z[j] <= '9')
                    break;
                z[j] = '0';
                if (j == 0) {
                    p.zBuf[static_cast<std::size_t>(i--)] = '1';
                    ++p.n;
                    ++p.iDP;
                    break;
                }
                --j;
            }
        }
    }
    p.z = &p.zBuf[static_cast<std::size_t>(i + 1)];
    while (p.n > 0 && p.z[p.n - 1] == '0')
        --p.n;
}

void appendThousands(std::string& s, std::size_t digitsBegin, std::size_t digitsEnd)
{
    if (digitsEnd <= digitsBegin + 3)
        return;
    std::string withCommas;
    withCommas.reserve(s.size() + (digitsEnd - digitsBegin) / 3);
    withCommas.append(s.substr(0, digitsBegin));
    const std::size_t digits = digitsEnd - digitsBegin;
    std::size_t first = digits % 3;
    if (first == 0)
        first = 3;
    withCommas.append(s.substr(digitsBegin, first));
    for (std::size_t pos = digitsBegin + first; pos < digitsEnd; pos += 3) {
        withCommas += ',';
        withCommas.append(s.substr(pos, 3));
    }
    withCommas.append(s.substr(digitsEnd));
    s.swap(withCommas);
}

std::string formatFloat(double real, char conv, int width, int precision, bool left, bool plus, bool blank,
                        bool zeropad, bool alternate, bool alt2, bool comma)
{
    if (precision < 0)
        precision = 6;
    int xtype = 0;  // 0=f, 1=e, 2=g
    if (conv == 'e' || conv == 'E')
        xtype = 1;
    else if (conv == 'g' || conv == 'G')
        xtype = 2;
    int iRound = 0;
    if (xtype == 0)
        iRound = -precision;
    else if (xtype == 2)
        iRound = precision;
    else
        iRound = precision + 1;
    FpDecode fp;
    fpDecode(fp, real, iRound, alt2 ? 26 : 16);
    if (fp.isSpecial != 0) {
        if (fp.isSpecial == 2)
            return zeropad ? "null" : "NaN";
        if (zeropad) {
            fp.zBuf[0] = '9';
            fp.z = fp.zBuf;
            fp.iDP = 1000;
            fp.n = 1;
        } else {
            std::string inf;
            if (fp.sign == '-')
                inf += '-';
            else if (plus)
                inf += '+';
            else if (blank)
                inf += ' ';
            inf += "Inf";
            return inf;
        }
    }
    char prefix = 0;
    if (fp.sign == '-')
        prefix = '-';
    else if (plus)
        prefix = '+';
    else if (blank)
        prefix = ' ';

    int exp = fp.iDP - 1;
    if (xtype == 2 && precision > 0)
        --precision;
    bool rtz = false;
    if (xtype == 2) {
        rtz = !alternate;
        if (exp < -4 || exp > precision) {
            xtype = 1;
        } else {
            precision -= exp;
            xtype = 0;
        }
    } else {
        rtz = alt2;
    }
    int e2 = xtype == 1 ? 0 : fp.iDP - 1;

    std::string s;
    s.reserve(static_cast<std::size_t>(std::max(width, precision + 32)));
    if (prefix != 0)
        s += prefix;
    const std::size_t digitsBegin = s.size();
    int j = 0;
    if (e2 < 0) {
        s += '0';
    } else {
        for (; e2 >= 0; --e2) {
            s += j < fp.n ? fp.z[j++] : '0';
        }
    }
    std::size_t digitsEnd = s.size();
    if (comma)
        appendThousands(s, digitsBegin, digitsEnd);
    const bool decimalPoint = precision > 0 || alternate || alt2;
    if (decimalPoint)
        s += '.';
    for (++e2; e2 < 0 && precision > 0; --precision, ++e2)
        s += '0';
    while (precision-- > 0)
        s += j < fp.n ? fp.z[j++] : '0';
    if (rtz && decimalPoint) {
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.') {
            if (alt2)
                s += '0';
            else
                s.pop_back();
        }
    }
    if (xtype == 1) {
        exp = fp.iDP - 1;
        s += (conv == 'E' || conv == 'G') ? 'E' : 'e';
        if (exp < 0) {
            s += '-';
            exp = -exp;
        } else {
            s += '+';
        }
        if (exp >= 100) {
            s += char('0' + (exp / 100));
            exp %= 100;
        }
        s += char('0' + (exp / 10));
        s += char('0' + (exp % 10));
    }
    if (zeropad && !left && static_cast<int>(s.size()) < width) {
        const int pads = width - static_cast<int>(s.size());
        const std::size_t at = prefix != 0 ? 1U : 0U;
        s.insert(at, static_cast<std::size_t>(pads), '0');
    }
    return s;
}

std::string formatInteger(std::uint64_t value, bool negative, int base, bool upper, int precision, int width,
                          bool zeropad, bool alternate, bool plus, bool blank, bool comma, char conv)
{
    static constexpr char lowerDigits[] = "0123456789abcdef";
    static constexpr char upperDigits[] = "0123456789ABCDEF";
    const char* digits = upper ? upperDigits : lowerDigits;
    if (value == 0)
        alternate = false;
    std::string prefix;
    int signChars = 0;
    if (negative)
        prefix += '-';
    else if (base == 10 && (conv == 'd' || conv == 'i')) {
        if (plus)
            prefix += '+';
        else if (blank)
            prefix += ' ';
    }
    signChars = prefix.empty() ? 0 : 1;
    if (alternate) {
        if (base == 8)
            prefix += '0';
        else if (base == 16)
            prefix += upper ? "0X" : "0x";
    }
    if (zeropad && precision < width - signChars)
        precision = width - signChars;
    char temp[80];
    int n = 0;
    do {
        temp[n++] = digits[value % static_cast<std::uint64_t>(base)];
        value /= static_cast<std::uint64_t>(base);
    } while (value != 0);
    std::string outDigits;
    outDigits.reserve(static_cast<std::size_t>(std::max(n, precision)));
    while (precision > n) {
        outDigits += '0';
        --precision;
    }
    for (int i = n - 1; i >= 0; --i)
        outDigits += temp[i];
    if (comma && (conv == 'd' || conv == 'i' || conv == 'u'))
        appendThousands(outDigits, 0, outDigits.size());
    return prefix + outDigits;
}

void appendEscaped(std::string_view arg, bool isNull, char quote, bool quoteSql, bool alt2, int precision,
                   int width, bool left, std::string& out)
{
    std::string text;
    if (isNull)
        text = quoteSql ? "NULL" : "(NULL)";
    else
        text.assign(arg.data(), arg.size());
    std::size_t bytes = text.size();
    if (precision >= 0)
        bytes = alt2 ? bytesForUtf8Chars(text, precision) : std::min<std::size_t>(bytes, precision);
    std::string escaped;
    if (!isNull && quoteSql)
        escaped += quote;
    for (std::size_t i = 0; i < bytes; ++i) {
        escaped += text[i];
        if (text[i] == quote)
            escaped += quote;
    }
    if (!isNull && quoteSql)
        escaped += quote;
    appendPadded(escaped, width, left, false, out);
}

bool sqlitePrintfImpl(FuncContext& ctx, std::string_view format, FormatArg& args, std::string& out)
{
    (void)ctx;
    const std::size_t initialSize = out.size();
    for (std::size_t pos = 0; pos < format.size() && format[pos] != '\0'; ++pos) {
        if (format[pos] != '%') {
            const std::size_t start = pos;
            while (pos < format.size() && format[pos] != '%' && format[pos] != '\0')
                ++pos;
            out.append(format.substr(start, pos - start));
            if (pos >= format.size() || format[pos] == '\0')
                break;
        }
        if (pos + 1 >= format.size() || format[pos + 1] == '\0') {
            out += '%';
            break;
        }
        ++pos;
        bool left = false;
        bool plus = false;
        bool blank = false;
        bool alternate = false;
        bool alt2 = false;
        bool zeropad = false;
        bool comma = false;
        int width = 0;
        int precision = -1;
        bool done = false;
        char c = format[pos];
        do {
            switch (c) {
                case '-': left = true; break;
                case '+': plus = true; break;
                case ' ': blank = true; break;
                case '#': alternate = true; break;
                case '!': alt2 = true; break;
                case '0': zeropad = true; break;
                case ',': comma = true; break;
                case 'l':
                    if (pos + 1 < format.size() && format[pos + 1] == 'l')
                        ++pos;
                    done = true;
                    break;
                case '*': {
                    width = static_cast<int>(getIntArg(args));
                    if (width < 0) {
                        left = true;
                        width = width >= -2147483647 ? -width : 0;
                    }
                    if (pos + 1 < format.size() && format[pos + 1] != '.' && format[pos + 1] != 'l') {
                        c = format[++pos];
                        done = true;
                    }
                    break;
                }
                case '.': {
                    if (pos + 1 >= format.size()) {
                        precision = 0;
                        done = true;
                        break;
                    }
                    c = format[++pos];
                    if (c == '*') {
                        precision = static_cast<int>(getIntArg(args));
                        if (precision < 0)
                            precision = precision >= -2147483647 ? -precision : -1;
                        if (pos + 1 < format.size())
                            c = format[++pos];
                    } else {
                        unsigned px = 0;
                        while (c >= '0' && c <= '9') {
                            px = px * 10 + static_cast<unsigned>(c - '0');
                            if (pos + 1 >= format.size())
                                break;
                            c = format[++pos];
                        }
                        precision = static_cast<int>(px & 0x7fffffffU);
                    }
                    if (c == 'l')
                        --pos;
                    else
                        done = true;
                    break;
                }
                case '1': case '2': case '3': case '4': case '5':
                case '6': case '7': case '8': case '9': {
                    unsigned wx = static_cast<unsigned>(c - '0');
                    while (pos + 1 < format.size() && format[pos + 1] >= '0' && format[pos + 1] <= '9') {
                        c = format[++pos];
                        wx = wx * 10 + static_cast<unsigned>(c - '0');
                    }
                    width = static_cast<int>(wx & 0x7fffffffU);
                    if (pos + 1 >= format.size()) {
                        done = true;
                    } else if (format[pos + 1] != '.' && format[pos + 1] != 'l') {
                        c = format[++pos];
                        done = true;
                    }
                    break;
                }
                default: done = true; break;
            }
            if (!done) {
                if (pos + 1 >= format.size())
                    return true;
                c = format[++pos];
            }
        } while (!done);

        std::string rendered;
        bool renderedUsesUtf8Width = false;
        switch (c) {
            case 'd':
            case 'i': {
                const std::int64_t v = getIntArg(args);
                const bool neg = v < 0;
                const std::uint64_t u = neg ? std::uint64_t(~v) + 1U : static_cast<std::uint64_t>(v);
                rendered = formatInteger(u, neg, 10, false, precision, width, zeropad, false, plus, blank, comma, c);
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                const std::uint64_t v = static_cast<std::uint64_t>(getIntArg(args));
                const int base = c == 'o' ? 8 : (c == 'x' || c == 'X' ? 16 : 10);
                rendered = formatInteger(v, false, base, c == 'X', precision, width, zeropad, alternate, false, false,
                                         comma, c);
                break;
            }
            case 'f':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
                rendered = formatFloat(getDoubleArg(args), c, width, precision, left, plus, blank, zeropad, alternate,
                                       alt2, comma);
                break;
            case 's':
            case 'z': {
                std::string_view s = getTextArg(args);
                if (precision >= 0)
                    s = s.substr(0, alt2 ? bytesForUtf8Chars(s, precision)
                                         : std::min<std::size_t>(s.size(), precision));
                rendered.assign(s.data(), s.size());
                renderedUsesUtf8Width = alt2;
                break;
            }
            case 'q': {
                const bool missing = args.used >= args.argc || args.argv[args.used]->isNull();
                std::string_view s = getTextArg(args);
                appendEscaped(s, missing, '\'', false, alt2, precision, width, left, out);
                continue;
            }
            case 'Q': {
                const bool missing = args.used >= args.argc || args.argv[args.used]->isNull();
                std::string_view s = getTextArg(args);
                appendEscaped(s, missing, '\'', true, alt2, precision, width, left, out);
                continue;
            }
            case 'w': {
                const bool missing = args.used >= args.argc || args.argv[args.used]->isNull();
                std::string_view s = getTextArg(args);
                appendEscaped(s, missing, '"', false, alt2, precision, width, left, out);
                continue;
            }
            case 'c': {
                std::string_view s = getTextArg(args);
                const std::size_t n = firstUtf8CharBytes(s);
                if (n == 0)
                    rendered.assign(1, '\0');
                else
                    rendered.assign(s.data(), n);
                if (precision > 1) {
                    std::string one = rendered;
                    rendered.clear();
                    for (int i = 0; i < precision; ++i)
                        rendered += one;
                    width -= precision - 1;
                }
                renderedUsesUtf8Width = true;
                break;
            }
            case '%':
                rendered = "%";
                break;
            case 'n':
                continue;
            default:
                return out.size() != initialSize;
        }
        appendPadded(rendered, width, left, renderedUsesUtf8Width, out);
    }
    return true;
}

void printfFunc(FuncContext& ctx, int argc, Args argv, Value& out)
{
    if (argc < 1 || argv[0]->isNull()) {
        out.setNull();
        return;
    }
    std::string formatScratch;
    std::string argScratch;
    const std::string_view format = textOf(*argv[0], formatScratch);
    ctx.buf.clear();
    FormatArg args{argc - 1, argv + 1, 0, &argScratch};
    if (!sqlitePrintfImpl(ctx, format, args, ctx.buf)) {
        out.setNull();
        return;
    }
    out.setText(ctx.buf);
}

}  // namespace

void sqlitePrintf(FuncContext& ctx, std::string_view format, int argc, Args argv, std::string& out)
{
    std::string argScratch;
    FormatArg args{argc, argv, 0, &argScratch};
    (void)sqlitePrintfImpl(ctx, format, args, out);
}

void registerPrintfFunctions(FunctionRegistry& r)
{
    r.add(FunctionDef{"printf", 0, -1, kDeterministic, printfFunc, nullptr});
    r.add(FunctionDef{"format", 0, -1, kDeterministic, printfFunc, nullptr});
}

}  // namespace sql::internal
