#!/usr/bin/env python3
import binascii
import itertools
import math
import sqlite3
from pathlib import Path


def hx(text: str) -> str:
    return binascii.hexlify(text.encode("utf-8", "surrogatepass")).decode("ascii")


def enc_arg(value) -> str:
    if value is None:
        return "N:"
    if isinstance(value, int):
        return f"I:{value}"
    if isinstance(value, float):
        if math.isnan(value):
            return "N:"
        if math.isinf(value):
            return "R:" + ("inf" if value > 0 else "-inf")
        return "R:" + repr(value)
    return "T:" + hx(str(value))


def main() -> None:
    conn = sqlite3.connect(":memory:")
    cases: list[str] = []
    formats = [
        "%d", "%+d", "% d", "%05d", "%-05d", "%05.3d", "%,d", "%0,12d", "%u", "%x", "%#08x",
        "%#x", "%X", "%#X", "%o", "%#o",
        "%f", "%.3f", "%08.2f", "%,f", "%e", "%E", "%g", "%.20g", "%!.20g", "%#g", "%!g",
        "%s", "%.3s", "%!3s", "%!.2s", "%-6s", "%z", "%q", "%Q", "%w", "%.4q", "%!.2Q",
        "%c", "%.3c", "%5c", "literal %% percent", "%*.*f", "%*s", "%.*s", "%nX", "%y after invalid",
    ]
    values = [
        None,
        0,
        1,
        -1,
        42,
        -123456789,
        9223372036854775807,
        -9223372036854775808,
        0.0,
        -0.0,
        0.1,
        1.23456789,
        1.0e20,
        1.0e-8,
        float("inf"),
        float("-inf"),
        float("nan"),
        "",
        "abc",
        "123abc",
        "O'Reilly",
        'a"b',
        "éclair",
    ]
    width_prec_prefixes = [
        (8, 3),
        (-8, 2),
        (0, -3),
    ]
    for fmt in formats:
        if "*" in fmt:
            for prefix in width_prec_prefixes:
                args = list(prefix if fmt.count("*") == 2 else prefix[:1]) + ["éclair"]
                row = conn.execute("SELECT typeof(printf(" + ",".join("?" for _ in [fmt, *args]) + ")), "
                                   "printf(" + ",".join("?" for _ in [fmt, *args]) + ")",
                                   [fmt, *args, fmt, *args]).fetchone()
                typ, value = row
                cases.append("\t".join(["printf", typ, hx(value), "T:" + hx(fmt), *map(enc_arg, args)]))
            continue
        for value in values:
            args = [fmt, value]
            row = conn.execute("SELECT typeof(printf(?,?)), printf(?,?)", args + args).fetchone()
            typ, result = row
            cases.append("\t".join(["printf", typ, "" if result is None else hx(result), "T:" + hx(fmt),
                                    enc_arg(value)]))

    combo_formats = ["%d|%s|%Q|%!.20g", "%2$d", "%d %d %d", "%*d %.*s"]
    combo_args = [
        [123, "abc", "O'Reilly", 0.1],
        [1],
        [],
        [6, 42, 2, "éclair"],
    ]
    for fmt, args in itertools.product(combo_formats, combo_args):
        params = [fmt, *args]
        row = conn.execute("SELECT typeof(printf(" + ",".join("?" for _ in params) + ")), "
                           "printf(" + ",".join("?" for _ in params) + ")",
                           params + params).fetchone()
        typ, result = row
        cases.append("\t".join(["printf", typ, "" if result is None else hx(result), "T:" + hx(fmt),
                                *map(enc_arg, args)]))
        alias = conn.execute("SELECT typeof(format(" + ",".join("?" for _ in params) + ")), "
                             "format(" + ",".join("?" for _ in params) + ")",
                             params + params).fetchone()
        cases.append("\t".join(["format", alias[0], "" if alias[1] is None else hx(alias[1]), "T:" + hx(fmt),
                                *map(enc_arg, args)]))

    cases.append("\t".join(["printf", "null", "", "N:"]))
    cases.append("\t".join(["printf", "null", ""]))
    cases.append("\t".join(["format", "null", ""]))
    Path("printf_cases.tsv").write_text("\n".join(cases) + "\n", encoding="utf-8")
    print(f"wrote {len(cases)} printf cases to printf_cases.tsv")


if __name__ == "__main__":
    main()
