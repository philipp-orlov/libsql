# libsql conformance harness

This directory contains test-only tools that compare libsql behavior with SQLite
3.45.1. The SQLite amalgamation is downloaded by CMake into the build tree and
is linked only into these conformance executables.

## Build

```sh
cmake -S /home/transcore/projects/nns/external/libsql \
      -B /home/transcore/projects/nns/external/libsql/build-conf \
      -DCMAKE_BUILD_TYPE=Release \
      -DSQL_BUILD_CONFORMANCE=ON
cmake --build /home/transcore/projects/nns/external/libsql/build-conf --target slt_runner difftest
```

Run the smoke test:

```sh
ctest --test-dir /home/transcore/projects/nns/external/libsql/build-conf \
      -L conformance --output-on-failure
```

## Fetch sqllogictest

```sh
/home/transcore/projects/nns/external/libsql/tools/fetch-sqllogictest.sh \
    /home/transcore/projects/nns/external/libsql/build-conf/slt
```

The script is idempotent and leaves the corpus at `build-conf/slt/test`.

## Run sqllogictest

Self-check the runner against the SQLite oracle:

```sh
/home/transcore/projects/nns/external/libsql/build-conf/tests/conformance/slt_runner \
    --engine sqlite \
    /home/transcore/projects/nns/external/libsql/build-conf/slt/test/select1.test \
    /home/transcore/projects/nns/external/libsql/build-conf/slt/test/select2.test \
    /home/transcore/projects/nns/external/libsql/build-conf/slt/test/select3.test \
    /home/transcore/projects/nns/external/libsql/build-conf/slt/test/evidence \
    /home/transcore/projects/nns/external/libsql/build-conf/slt/test/index/between
```

Measure libsql on the same subset:

```sh
/home/transcore/projects/nns/external/libsql/build-conf/tests/conformance/slt_runner \
    --engine libsql \
    --skip-list /home/transcore/projects/nns/external/libsql/tests/conformance/skiplist.txt \
    /home/transcore/projects/nns/external/libsql/build-conf/slt/test/select1.test \
    /home/transcore/projects/nns/external/libsql/build-conf/slt/test/select2.test \
    /home/transcore/projects/nns/external/libsql/build-conf/slt/test/select3.test \
    /home/transcore/projects/nns/external/libsql/build-conf/slt/test/evidence \
    /home/transcore/projects/nns/external/libsql/build-conf/slt/test/index/between
```

Useful options:

- `--verbose` prints SQL, expected output, and actual output for failures.
- `--max-failures N` stops after N unexpected failures.
- `--summary-json FILE` writes a machine-readable summary.
- `--skip-list FILE` skips known failures. Lines are
  `<file-glob-or-path>[:<line>] <reason>`.

## Run differential tests

Compare one or more SQL scripts statement by statement:

```sh
/home/transcore/projects/nns/external/libsql/build-conf/tests/conformance/difftest script.sql
```

Run the random generator and write minimized reproducers for mismatches:

```sh
mkdir -p /home/transcore/projects/nns/external/libsql/build-conf/repros
/home/transcore/projects/nns/external/libsql/build-conf/tests/conformance/difftest \
    --fuzz --seed 1 --count 300 \
    --out /home/transcore/projects/nns/external/libsql/build-conf/repros
```

The fuzzer avoids nondeterministic SQLite features such as `random()` and
`now`. Query results without top-level `ORDER BY` are compared as row multisets.
After DML, the harness compares `SELECT * FROM <table> ORDER BY <all columns>`
to catch state divergence.
A result that differs from SQLite's only in the order of rows whose `ORDER BY`
keys tie as an INTEGER against an equal REAL (`1` and `1.0` compare equal, so
either order is right) is counted as a tie order, not a mismatch: the rows
must be the same multiset and equal position by position up to that.

The scripts in `scripts/` are run by the `conformance_scripts` test and cover
triggers, foreign keys, views, transactions, windows, covering-index reads and
exact range windows, text holding NUL bytes, ORDER BY with LIMIT, and WITH in
front of INSERT, UPDATE and DELETE.

## Long fuzz runs

`fuzz-long.sh` runs the generator over consecutive seeds on several workers
until a deadline, keeping a repro for every seed with a hard mismatch:

```sh
cp build-conf/tests/conformance/difftest /some/stable/place/difftest
tests/conformance/fuzz-long.sh /some/stable/place/difftest /some/stable/place/out 24 8 2000
```

That is 24 hours on 8 workers at 2,000 statements per seed. `out/summary.txt`
is rewritten every minute (seeds run, seeds with hard mismatches, tie orders),
`out/run.log` has one line per seed, and `out/mismatch-<seed>.txt` plus
`out/repro-<seed>/` hold anything found. `FUZZ_SECONDS` overrides the duration
for a quick try. Copy the binary out of the build tree first so that a rebuild
does not change it mid-run.
