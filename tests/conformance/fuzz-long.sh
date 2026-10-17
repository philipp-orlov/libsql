#!/usr/bin/env bash
# Long differential fuzz run against SQLite: `difftest --fuzz` over consecutive
# seeds, on several workers, until the time is up. Every hard mismatch keeps
# its minimized repro script under OUT/repro-<seed>/.
#
#   fuzz-long.sh DIFFTEST OUT [HOURS] [WORKERS] [COUNT]
#
# DIFFTEST is the difftest binary, OUT a directory for the log and repros,
# HOURS defaults to 24, WORKERS to 8, COUNT (statements per seed) to 2000.
# Copy the binary somewhere stable first if the build tree may be rebuilt
# while this runs. OUT/summary.txt is rewritten every minute. FUZZ_SECONDS
# overrides the duration (for a quick try).
set -u
difftest=${1:?difftest binary}
out=${2:?output directory}
hours=${3:-24}
workers=${4:-8}
count=${5:-2000}

mkdir -p "$out"
deadline=$(( $(date +%s) + ${FUZZ_SECONDS:-$(( hours * 3600 ))} ))
start=$(date +%s)
echo "start $(date -Is) hours=$hours workers=$workers count=$count binary=$difftest" > "$out/run.log"

worker() {
    local w=$1 seed=$1
    while [ "$(date +%s)" -lt "$deadline" ]; do
        local line
        line=$(nice -n 10 "$difftest" --fuzz --seed "$seed" --count "$count" --out "$out/repro-$seed" 2>&1 \
               | tee "$out/last-$w.txt" | tail -n 1)
        echo "seed $seed: $line" >> "$out/run.log"
        case "$line" in
            *" 0 hard mismatches"*) rm -rf "$out/repro-$seed" ;;
            *) cp "$out/last-$w.txt" "$out/mismatch-$seed.txt" ;;
        esac
        seed=$(( seed + workers ))
    done
}

for (( w = 1; w <= workers; ++w )); do
    worker "$((1000 + w))" &
done

summarize() {
    local seeds hard ties
    seeds=$(grep -c '^seed ' "$out/run.log")
    hard=$(grep '^seed ' "$out/run.log" | grep -vc ' 0 hard mismatches')
    ties=$(grep -o '[0-9]* INTEGER/REAL tie orders' "$out/run.log" | awk '{s += $1} END {print s + 0}')
    printf 'elapsed %ss, seeds %s (x%s statements), seeds with hard mismatches %s, tie orders %s\n' \
        "$(( $(date +%s) - start ))" "$seeds" "$count" "$hard" "$ties" > "$out/summary.txt"
}
while [ "$(date +%s)" -lt "$deadline" ]; do
    summarize
    sleep 60
done
wait
summarize
echo "end $(date -Is)" >> "$out/run.log"
