#!/usr/bin/env python3
import base64
import math
import os
import sqlite3
import sys


EXPECTED_SQLITE = "3.45.1"


def b64e(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def enc_arg(value, subtype=0):
    if value is None:
        return "N,0,"
    if isinstance(value, bytes):
        return f"B,{subtype},{b64e(value)}"
    if isinstance(value, bool):
        return f"I,{subtype},{int(value)}"
    if isinstance(value, int):
        return f"I,{subtype},{value}"
    if isinstance(value, float):
        if math.isnan(value):
            payload = "nan"
        elif math.isinf(value):
            payload = "inf" if value > 0 else "-inf"
        else:
            payload = repr(value)
        return f"R,{subtype},{payload}"
    return f"T,{subtype},{b64e(str(value).encode('utf-8'))}"


def emit_value(value, subtype=0):
    if value is None:
        return f"N,{subtype},"
    if isinstance(value, bytes):
        return f"B,{subtype},{b64e(value)}"
    if isinstance(value, int):
        return f"I,{subtype},{value}"
    if isinstance(value, float):
        if math.isnan(value):
            payload = "nan"
        elif math.isinf(value):
            payload = "inf" if value > 0 else "-inf"
        else:
            payload = repr(value)
        return f"R,{subtype},{payload}"
    return f"T,{subtype},{b64e(str(value).encode('utf-8'))}"


def is_value_arg(name, index):
    if name in {"json_array", "jsonb_array", "json_quote"}:
        return True
    if name in {"json_object", "jsonb_object"}:
        return index % 2 == 1
    if name in {"json_insert", "jsonb_insert", "json_replace", "jsonb_replace", "json_set", "jsonb_set"}:
        return index >= 2 and index % 2 == 0
    return False


def expr_for_arg(name, index, arg, params, value_position=None):
    typ, subtype, payload = arg
    if typ == "N":
        value = None
    elif typ == "I":
        value = int(payload)
    elif typ == "R":
        if payload == "inf":
            value = math.inf
        elif payload == "-inf":
            value = -math.inf
        elif payload == "nan":
            value = math.nan
        else:
            value = float(payload)
    elif typ == "T":
        value = base64.b64decode(payload).decode("utf-8")
    elif typ == "B":
        value = sqlite3.Binary(base64.b64decode(payload))
    else:
        raise AssertionError(typ)
    params.append(value)
    raw_value = is_value_arg(name, index) if value_position is None else value_position
    if raw_value and typ == "T" and subtype == 74:
        return "json(?)"
    return "?"


def parse_arg(spec):
    parts = spec.split(",", 2)
    if len(parts) < 3:
        parts.append("")
    return parts[0], int(parts[1]), parts[2]


def result_subtype(name, argc, result):
    if not isinstance(result, str):
        return 0
    if name in {
        "json",
        "json_array",
        "json_object",
        "json_insert",
        "json_replace",
        "json_set",
        "json_remove",
        "json_patch",
        "json_quote",
        "json_group_array",
        "json_group_object",
        "->",
    }:
        return 74
    if name == "json_extract":
        stripped = result.lstrip()
        return 74 if argc > 2 or stripped.startswith(("[", "{")) else 0
    return 0


def scalar_expected(con, name, args):
    parsed = [parse_arg(a) for a in args]
    params = []
    try:
        if name in {"->", "->>"}:
            left = expr_for_arg(name, 0, parsed[0], params, False)
            right = expr_for_arg(name, 1, parsed[1], params, False)
            sql = f"SELECT ({left}) {name} ({right})"
        else:
            exprs = [expr_for_arg(name, i, arg, params) for i, arg in enumerate(parsed)]
            sql = f"SELECT {name}({','.join(exprs)})"
        result = con.execute(sql, params).fetchone()[0]
        return "OK", emit_value(result, result_subtype(name, len(args), result))
    except Exception as exc:
        return "ERR", b64e(str(exc).encode("utf-8"))


def aggregate_expected(con, name, rows):
    params = []
    is_object = name in {"json_group_object", "jsonb_group_object"}
    try:
        if not rows:
            if is_object:
                sql = f"SELECT {name}(k,v) FROM (SELECT NULL AS k,NULL AS v WHERE 0)"
            else:
                sql = f"SELECT {name}(x) FROM (SELECT NULL AS x WHERE 0)"
            result = con.execute(sql).fetchone()[0]
            return "OK", emit_value(result, result_subtype(name, 1, result))
        selects = []
        for row in rows:
            parsed = [parse_arg(a) for a in row]
            if is_object:
                key = expr_for_arg(name, 0, parsed[0], params, False)
                value = expr_for_arg(name, 1, parsed[1], params, True)
                selects.append(f"SELECT {key} AS k,{value} AS v")
            else:
                value = expr_for_arg(name, 0, parsed[0], params, True)
                selects.append(f"SELECT {value} AS x")
        if is_object:
            sql = f"SELECT {name}(k,v) FROM ({' UNION ALL '.join(selects)})"
        else:
            sql = f"SELECT {name}(x) FROM ({' UNION ALL '.join(selects)})"
        result = con.execute(sql, params).fetchone()[0]
        return "OK", emit_value(result, result_subtype(name, 1, result))
    except Exception as exc:
        return "ERR", b64e(str(exc).encode("utf-8"))


def table_expected(con, name, args):
    parsed = [parse_arg(a) for a in args]
    params = []
    try:
        json_expr = expr_for_arg(name, 0, parsed[0], params, False)
        if len(parsed) == 1:
            sql = f"SELECT rowid,key,value,type,atom,id,parent,fullkey,path,json,root FROM {name}({json_expr})"
        else:
            root_expr = expr_for_arg(name, 1, parsed[1], params, False)
            sql = f"SELECT rowid,key,value,type,atom,id,parent,fullkey,path,json,root FROM {name}({json_expr},{root_expr})"
        rows = con.execute(sql, params).fetchall()
        encoded_rows = []
        for row in rows:
            fields = [str(row[0])]
            fields.extend(emit_value(value) for value in row[1:])
            encoded_rows.append("|".join(fields))
        return "OK", b64e("\n".join(encoded_rows).encode("utf-8"))
    except Exception as exc:
        return "ERR", b64e(str(exc).encode("utf-8"))


def add_scalar(lines, con, name, *args):
    status, expected = scalar_expected(con, name, list(args))
    lines.append("\t".join(["S", name, str(len(args)), *args, status, expected]))


def add_aggregate(lines, con, name, rows):
    argc = 2 if "object" in name else 1
    status, expected = aggregate_expected(con, name, rows)
    row_text = ";".join("|".join(row) for row in rows)
    lines.append("\t".join(["A", name, str(argc), b64e(row_text.encode("utf-8")), status, expected]))


def add_table(lines, con, name, *args):
    status, expected = table_expected(con, name, list(args))
    lines.append("\t".join(["T", name, str(len(args)), *args, status, expected]))


def main():
    if sqlite3.sqlite_version != EXPECTED_SQLITE:
        raise SystemExit(f"sqlite3 {sqlite3.sqlite_version}, expected {EXPECTED_SQLITE}")
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "json_cases.tsv")
    con = sqlite3.connect(":memory:")
    lines = ["# kind, name, argc, args..., status, expected"]

    json_texts = [
        "{}",
        "[]",
        '{"a":1,"b":[true,false,null],"c":"x"}',
        "[1,2,3]",
        "{a:1, b:'two', c:[1,2,], /*x*/ d:+Infinity}",
        '{"unicode":"☃","esc":"\\n\\u0041","big":9223372036854775807}',
    ]
    paths = ["$", "$.a", "$.b", "$.b[0]", "$.b[#-1]", "$.missing", "$.\"unicode\""]
    values = [
        enc_arg(None),
        enc_arg(0),
        enc_arg(1),
        enc_arg(-7),
        enc_arg(1.25),
        enc_arg(math.inf),
        enc_arg("text"),
        enc_arg("[9,8]", 74),
        enc_arg('{"raw":true}', 74),
        enc_arg(con.execute("SELECT jsonb(?)", ('{"jb":1}',)).fetchone()[0]),
    ]

    for text in json_texts:
        text_arg = enc_arg(text)
        add_scalar(lines, con, "json", text_arg)
        add_scalar(lines, con, "jsonb", text_arg)
        add_scalar(lines, con, "json_error_position", text_arg)
        for flag in [1, 2, 4, 5, 6, 8, 9, 10, 15]:
            add_scalar(lines, con, "json_valid", text_arg, enc_arg(flag))
        for path in paths:
            add_scalar(lines, con, "json_extract", text_arg, enc_arg(path))
            add_scalar(lines, con, "jsonb_extract", text_arg, enc_arg(path))
            add_scalar(lines, con, "json_type", text_arg, enc_arg(path))
            add_scalar(lines, con, "json_array_length", text_arg, enc_arg(path))
            add_scalar(lines, con, "->", text_arg, enc_arg(path if path.startswith("$") else path[2:]))
            add_scalar(lines, con, "->>", text_arg, enc_arg(path if path.startswith("$") else path[2:]))
        add_scalar(lines, con, "json_extract", text_arg, enc_arg("$.a"), enc_arg("$.b"))
        add_scalar(lines, con, "json_remove", text_arg, enc_arg("$.a"), enc_arg("$.missing"))
        add_scalar(lines, con, "jsonb_remove", text_arg, enc_arg("$.a"))
        add_table(lines, con, "json_each", text_arg)
        add_table(lines, con, "json_tree", text_arg)
        add_table(lines, con, "json_each", text_arg, enc_arg("$"))
        add_table(lines, con, "json_tree", text_arg, enc_arg("$"))

    for subset_size in range(0, len(values) + 1):
        subset = values[:subset_size]
        add_scalar(lines, con, "json_array", *subset)
        add_scalar(lines, con, "jsonb_array", *subset)
        object_args = []
        for index, value in enumerate(subset):
            object_args.extend([enc_arg(f"k{index}"), value])
        add_scalar(lines, con, "json_object", *object_args)
        add_scalar(lines, con, "jsonb_object", *object_args)

    for value in values:
        add_scalar(lines, con, "json_quote", value)
        add_scalar(lines, con, "json_set", enc_arg("{}"), enc_arg("$.x"), value)
        add_scalar(lines, con, "jsonb_set", enc_arg("{}"), enc_arg("$.x"), value)
        add_scalar(lines, con, "json_insert", enc_arg("{}"), enc_arg("$.x"), value)
        add_scalar(lines, con, "json_replace", enc_arg('{"x":0}'), enc_arg("$.x"), value)

    patch_inputs = [
        ("{}", "{}"),
        ('{"a":1}', '{"b":2}'),
        ('{"a":{"x":1},"b":2}', '{"a":{"y":3},"b":null}'),
        ("[1,2]", '{"a":1}'),
    ]
    for left, right in patch_inputs:
        add_scalar(lines, con, "json_patch", enc_arg(left), enc_arg(right))
        add_scalar(lines, con, "jsonb_patch", enc_arg(left), enc_arg(right))

    error_cases = [
        ("json", enc_arg("{")),
        ("json_object", enc_arg("a")),
        ("json_object", enc_arg(1), enc_arg(2)),
        ("json_extract", enc_arg("{}"), enc_arg("$x")),
        ("json_valid", enc_arg("{}"), enc_arg(0)),
        ("json_array", enc_arg(b"not-jsonb")),
    ]
    for name, *args in error_cases:
        add_scalar(lines, con, name, *args)

    aggregate_rows = [[value] for value in values]
    object_rows = [[enc_arg(f"k{i}"), value] for i, value in enumerate(values)]
    for name in ["json_group_array", "jsonb_group_array"]:
        add_aggregate(lines, con, name, [])
        add_aggregate(lines, con, name, aggregate_rows)
        add_aggregate(lines, con, name, aggregate_rows[2:7])
    for name in ["json_group_object", "jsonb_group_object"]:
        add_aggregate(lines, con, name, [])
        add_aggregate(lines, con, name, object_rows)
        add_aggregate(lines, con, name, object_rows[1:5])

    # Expand deterministic scalar coverage with many path/value combinations.
    for i in range(600):
        doc = enc_arg(f'{{"i":{i},"arr":[{i},{i+1},{{"x":{i % 7}}}],"s":"v{i}"}}')
        add_scalar(lines, con, "json_extract", doc, enc_arg("$.arr[2].x"))
        add_scalar(lines, con, "json_set", doc, enc_arg("$.new"), values[i % len(values)])
        add_scalar(lines, con, "json_insert", doc, enc_arg("$.arr[#]"), values[(i + 3) % len(values)])
        add_scalar(lines, con, "json_remove", doc, enc_arg("$.s"))
        add_scalar(lines, con, "json_type", doc, enc_arg("$.arr"))
        add_scalar(lines, con, "json_valid", doc)

    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
        f.write("\n")
    print(f"wrote {len(lines) - 1} cases to {out}")


if __name__ == "__main__":
    main()
