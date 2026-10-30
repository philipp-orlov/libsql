#!/usr/bin/env python3
import binascii
import itertools
import sqlite3
from pathlib import Path

NOW_TEXT = "2024-02-29 12:34:56.789"


def hx(text: str) -> str:
    return binascii.hexlify(text.encode("utf-8")).decode("ascii")


def arg_text(text: str) -> str:
    return "T:" + hx(text)


def arg_num(text: str) -> str:
    return ("R:" if any(c in text for c in ".eE") else "I:") + text


def expected(conn: sqlite3.Connection, func: str, args: list[str]) -> tuple[str, str]:
    placeholders = ",".join("?" for _ in args)
    sql = f"SELECT typeof({func}({placeholders})), {func}({placeholders})"
    row = conn.execute(sql, args * 2).fetchone()
    typ, value = row
    if value is None:
        return typ, ""
    if typ == "text":
        return typ, hx(value)
    if typ == "real":
        return typ, conn.execute("SELECT printf('%.17g', ?)", (value,)).fetchone()[0]
    return typ, str(value)


def emit_case(conn: sqlite3.Connection, func: str, args: list[str], encoded_args: list[str], out: list[str]) -> None:
    typ, value = expected(conn, func, args)
    out.append("\t".join([func, typ, value, *encoded_args]))


def main() -> None:
    conn = sqlite3.connect(":memory:")
    cases: list[str] = []

    bases = [
        "2024-02-29",
        "2024-02-29 12:34",
        "2024-02-29 12:34:56",
        "2024-02-29T12:34:56.789",
        "2024-02-29 12:34:56.789Z",
        "2024-02-29 07:34:56.789-05:00",
        "1999-12-31 23:59:59.999",
        "2000-01-01 24:01",
        "0000-01-01",
        "9999-12-31 23:59:59.999",
        "12:34:56.789",
        "2451545.0",
        "1709210096",
        "not-a-date",
        "2023-02-31",
    ]
    one_mods = [
        [],
        ["+1 day"],
        ["-2 hours"],
        ["+30 minutes"],
        ["+1.25 seconds"],
        ["+1 month"],
        ["-1 year"],
        ["start of day"],
        ["start of month"],
        ["start of year"],
        ["weekday 0"],
        ["weekday 4"],
        ["subsec"],
        ["subsecond"],
        ["localtime"],
        ["utc"],
    ]
    numeric_mods = [["unixepoch"], ["unixepoch", "subsec"], ["auto"], ["julianday"]]
    span_mods = [
        ["+0001-02-03"],
        ["-0001-02-03 04:05"],
        ["+0000-00-01 00:00:01.250"],
    ]

    for base in bases:
        for mods in one_mods + span_mods:
            args = [NOW_TEXT if base == "now" else base, *mods]
            enc = [arg_text(base), *map(arg_text, mods)]
            for func in ("date", "time", "datetime", "julianday", "unixepoch"):
                emit_case(conn, func, args, enc, cases)
        if base in {"1709210096", "2451545.0"}:
            for mods in numeric_mods:
                args = [base, *mods]
                enc = [arg_num(base), *map(arg_text, mods)]
                for func in ("date", "time", "datetime", "julianday", "unixepoch"):
                    emit_case(conn, func, args, enc, cases)

    formats = [
        "%Y-%m-%d",
        "%H:%M:%f",
        "%F %T",
        "%s",
        "%J",
        "%w %u %W %j",
        "%d|%e|%H|%k|%I|%l|%M|%S|%p|%P|%R|%%",
        "%G",
        "%V",
        "%U",
    ]
    for fmt, base, mods in itertools.product(formats, bases[:12], one_mods[:8]):
        args = [fmt, NOW_TEXT if base == "now" else base, *mods]
        enc = [arg_text(fmt), arg_text(base), *map(arg_text, mods)]
        emit_case(conn, "strftime", args, enc, cases)

    timediff_pairs = [
        ("2024-02-29 12:34:56.789", "2023-01-31 01:02:03.004"),
        ("2023-01-31", "2024-02-29"),
        ("2000-01-01 00:00:00", "2000-01-01 00:00:00.999"),
        ("2451545.0", "2451544.5"),
    ]
    for a, b in timediff_pairs:
        emit_case(conn, "timediff", [a, b], [arg_text(a), arg_text(b)], cases)

    for func in ("current_date", "current_time", "current_timestamp"):
        mapped = {"current_date": "date", "current_time": "time", "current_timestamp": "datetime"}[func]
        typ, value = expected(conn, mapped, [NOW_TEXT])
        cases.append("\t".join([func, typ, value]))

    Path("date_cases.tsv").write_text("\n".join(cases) + "\n", encoding="utf-8")
    print(f"wrote {len(cases)} date cases to date_cases.tsv")


if __name__ == "__main__":
    main()
