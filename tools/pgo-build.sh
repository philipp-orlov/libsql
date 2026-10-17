#!/usr/bin/env sh
# Builds libsql (and libnosql) with profile-guided optimization:
#   1. configure <builddir> with SQL_PGO=GENERATE and build the benchmarks,
#   2. run bench_sql as the training workload (profiles land in
#      <builddir>/pgo-data),
#   3. reconfigure the same tree with SQL_PGO=USE and build everything.
# The profile is tied to the build tree's paths: build and train in the tree
# you ship from. Extra arguments go to cmake's configure step.
#
#   tools/pgo-build.sh build-pgo -DSQL_BUILD_TESTS=ON
set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <builddir> [cmake args...]" >&2
    exit 2
fi
build=$1
shift
src=$(cd "$(dirname "$0")/.." && pwd)
jobs=$(nproc 2>/dev/null || echo 4)

rm -rf "$build/pgo-data"
cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release -DSQL_BUILD_BENCHMARKS=ON \
    -DSQL_PGO=GENERATE "$@"
cmake --build "$build" --target bench_sql -j "$jobs"

# The training run: every statement shape bench_sql has, at a size that
# reaches steady state without taking long.
"$build/benchmarks/bench_sql" --rows 50000 --reps 1 >/dev/null

cmake -S "$src" -B "$build" -DSQL_PGO=USE "$@"
cmake --build "$build" -j "$jobs"
echo "PGO build ready in $build"
