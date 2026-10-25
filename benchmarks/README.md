# Benchmarks

Executables, not tests: they print a table. Build with `-DSQL_BUILD_BENCHMARKS=ON` in Release and pin to a core.

## bench_bufs: binary results against JSON

`./bench_bufs [rows...]` compares `sql::BufsWriter` (see `include/sql/bufs.hpp`) with `sql::toJson` over a `Result` on
a generated table of `(id, name, status /*5 values*/, price /*2 decimals*/, qty /*0..1000*/, created /*seconds in one
year*/, note /*80% NULL*/)`, and on a one-column integer table. JSON here is `sql::toJson`, i.e. without the web API's
own per-cell encoding; the gzip rows need zlib. Median of 20 warm runs.

Measured 2026-10-25 on a Neoverse-N1 (aarch64, 2 cores), g++ 13.3.0, Release:

| Data set | Wire bytes, JSON | Wire bytes, bufs | Smaller | gzip -6 JSON | gzip -6 bufs | Smaller |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| orders, 10,000 rows x 7 | 1,136,765 | 381,672 | 2.98x | 221,033 | 165,806 | 1.33x |
| orders, 100,000 rows x 7 | 11,462,903 | 3,456,912 | 3.32x | 2,205,796 | 1,520,875 | 1.45x |
| one integer column, 1,000,000 rows | 12,888,897 | 4,000,208 | 3.22x | 2,362,643 | 1,383,161 | 1.71x |

| Data set | `exec` + JSON | `BufsWriter::exec` | `exec` + `write(Result)` | C++ reads every cell |
| --- | ---: | ---: | ---: | ---: |
| orders, 10,000 rows | 17.3 ms | 2.5 ms | 3.5 ms | 0.38 ms |
| orders, 100,000 rows | 180.9 ms | 31.0 ms | 47.1 ms | 4.0 ms |
| one integer column, 1,000,000 rows | 315.8 ms | 66.7 ms | 246.8 ms | 5.4 ms |

Memory: with `batchRows(10000)` and `onBatch` the writer holds one batch (100,000 orders rows: largest message 381,672
bytes) however large the result; a single-message writer holds the staging plus the output (about 18.8 MB for the same
100,000 rows, the JSON being 11.5 MB on top of a `Result`).

Per row of the 7-column set the message is about 35 bytes (id 4, name text + offset about 14, status 1, price 8 --
two-decimal values do not round-trip through float --, qty 2, created 4, note about 0.5), against about 115 for JSON.

### Client decode (Node 22, `tools/sql/ui/scripts/bench-decode.mjs`)

Against a live `sql --listen`, 100,000 rows x 7 columns (here `created` is text, so the wire gain is smaller than
above: JSON 6,985,069 bytes, bufs 5,702,608). Median of 15:

| What the client does | Time |
| --- | ---: |
| `JSON.parse` (the grid then holds `Cell[][]`) | 60.3 ms |
| `ResultSetView.open` + read the 50 rows the grid shows | 1.22 ms |
| `ResultSetView.open` + read every cell as a `Cell` | 146.9 ms |

The grid renders only the visible rows, so the first-paint cost drops about 50x. Materializing every cell (CSV
export, sorting a column) is slower than `JSON.parse`, because each cell is built on demand where the JSON path paid
up front.
