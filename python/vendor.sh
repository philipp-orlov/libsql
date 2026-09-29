#!/usr/bin/env bash
# Copies libsql's and libnosql's engine sources into python/_vendor so the
# Python package is self-contained: a wheel built from python/ alone (no
# sibling checkouts needed) for manylinux containers, CI runners, or any
# machine that only has this python/ directory.
#
# Run before building a distributable sdist/wheel:
#   ./vendor.sh && python -m build .        (or: cibuildwheel .)
#
# python/CMakeLists.txt prefers _vendor/libsql over ../.. when it exists.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

SQL_SRC="${SQL_SRC:-..}"
NOSQL_SRC="${NOSQL_SRC:-../../libnosql}"
DEST=_vendor

if [[ ! -f "$SQL_SRC/CMakeLists.txt" ]]; then
    echo "vendor.sh: no libsql checkout at $SQL_SRC (set SQL_SRC=...)" >&2
    exit 1
fi
if [[ ! -f "$NOSQL_SRC/CMakeLists.txt" ]]; then
    echo "vendor.sh: no libnosql checkout at $NOSQL_SRC (set NOSQL_SRC=...)" >&2
    exit 1
fi

rm -rf "$DEST"
mkdir -p "$DEST/libsql" "$DEST/libnosql"

# Only what the engine needs to build: headers, sources, the top CMakeLists.
for d in src include; do
    mkdir -p "$DEST/libsql/$d" "$DEST/libnosql/$d"
    cp -a "$SQL_SRC/$d/." "$DEST/libsql/$d/"
    cp -a "$NOSQL_SRC/$d/." "$DEST/libnosql/$d/"
done
cp -a "$SQL_SRC/CMakeLists.txt" "$DEST/libsql/CMakeLists.txt"
cp -a "$NOSQL_SRC/CMakeLists.txt" "$DEST/libnosql/CMakeLists.txt"
cp -a "$NOSQL_SRC/cmake" "$DEST/libnosql/cmake"

du -sh "$DEST" | sed 's/^/vendor.sh: /'
