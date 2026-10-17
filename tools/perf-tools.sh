#!/usr/bin/env sh
# Installs the profiling tools used for libsql performance work (see
# docs/internal/perf.md) on Ubuntu 24.04, and lets the current user run perf.
#
#   --gui      also install the viewers: hotspot, heaptrack-gui, kcachegrind
#   --persist  keep the perf permission across reboots (/etc/sysctl.d)
set -eu

gui=0
persist=0
for arg in "$@"; do
    case "$arg" in
    --gui) gui=1 ;;
    --persist) persist=1 ;;
    *)
        echo "usage: $0 [--gui] [--persist]" >&2
        exit 2
        ;;
    esac
done

sudo=
if [ "$(id -u)" -ne 0 ]; then
    sudo=sudo
fi

# perf must match the running kernel; heaptrack and valgrind find allocation
# hot spots; the allocators are for LD_PRELOAD comparisons.
packages="linux-tools-common linux-tools-$(uname -r) heaptrack valgrind gdb
libmimalloc-dev libjemalloc-dev"
if [ "$gui" -eq 1 ]; then
    packages="$packages hotspot heaptrack-gui kcachegrind"
fi

$sudo apt-get update
# shellcheck disable=SC2086
$sudo apt-get install -y $packages

# 1 lets users profile their own processes, user and kernel side, with
# hardware counters. kptr_restrict is left alone: user-space profiles do not
# need kernel addresses.
$sudo sysctl -w kernel.perf_event_paranoid=1
if [ "$persist" -eq 1 ]; then
    echo "kernel.perf_event_paranoid = 1" |
        $sudo tee /etc/sysctl.d/60-libsql-perf.conf >/dev/null
fi

cat <<'EOF'

Installed. Typical uses (bench_vs_sqlite needs SQL_BUILD_CONFORMANCE):
  perf record --call-graph=lbr -- build-conf/benchmarks/bench_vs_sqlite --only scan
  perf stat -M TopdownL1 -- build-conf/benchmarks/bench_vs_sqlite --only scan
  heaptrack build/benchmarks/bench_sql
  LD_PRELOAD=$(ls /usr/lib/x86_64-linux-gnu/libmimalloc.so.* | head -1) build-conf/benchmarks/bench_vs_sqlite
  valgrind --tool=dhat build/benchmarks/bench_sql
EOF
