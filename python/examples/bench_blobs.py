#!/usr/bin/env python3
"""Measure the blob archive the way a training job uses it.

    PYTHONPATH=build/python python3 python/examples/bench_blobs.py [--dir DIR]

Ingestion: members/s and MiB/s for small, medium and large payloads, with the
Writer's buffer on and off. Reads: a shuffled pass over the corpus as single
lookups (own transaction each), single lookups inside one Snapshot, batched
get_many, and get_many with the next batch prefetched.

Everything here runs against a warm page cache unless you drop it between
phases (`sync; echo 3 | sudo tee /proc/sys/vm/drop_caches`) or point --dir at
a corpus larger than RAM; prefetch pays off exactly when pages are cold.
"""

import argparse
import os
import shutil
import tempfile
import time

import numpy as np

import libsql


def fmt_rate(n, secs):
    return f"{n / secs:>12,.0f}"


def ingest(path, name, count, size, write_buffer):
    db = libsql.Database(path, durable=False)
    blobs = libsql.BlobStorage(db, name, write_buffer=write_buffer)
    payload = np.random.default_rng(0).integers(0, 256, size=size, dtype=np.uint8)
    t0 = time.perf_counter()
    with blobs.writer() as w:
        for i in range(count):
            w.add(i, payload)  # same bytes each time: the cost measured is the archive's, not RNG's
    dt = time.perf_counter() - t0
    mib = count * size / 2**20
    print(f"  {size / 1024:>7,.0f} KiB x {count:>7,}  buffer={write_buffer >> 20 or 0:>2} MiB  "
          f"{fmt_rate(count, dt)} members/s  {mib / dt:>9,.0f} MiB/s  ({dt:.2f}s)")
    del blobs
    db.close()


def reads(path, name, count, size, batch):
    ro = libsql.Database(path, read_only=True)
    blobs = libsql.BlobStorage(ro, name, access="random")
    order = np.random.default_rng(1).permutation(count)
    touched = 0

    def consume(b):
        nonlocal touched
        touched += memoryview(b)[0]  # force the first page in, as a model would

    print(f"  {size / 1024:>7,.0f} KiB x {count:>7,}, shuffled, batch {batch}")

    t0 = time.perf_counter()
    for k in order:
        consume(blobs[int(k)])
    dt = time.perf_counter() - t0
    print(f"    get(key), own snapshot each   {fmt_rate(count, dt)} lookups/s  {dt / count * 1e6:7.1f} us each")

    with blobs.snapshot() as snap:
        t0 = time.perf_counter()
        for k in order:
            consume(snap[int(k)])
        dt = time.perf_counter() - t0
        print(f"    snapshot[key]                 {fmt_rate(count, dt)} lookups/s  {dt / count * 1e6:7.1f} us each")

        batches = [[int(k) for k in order[i : i + batch]] for i in range(0, count, batch)]
        t0 = time.perf_counter()
        for keys in batches:
            for b in snap.get_many(keys):
                consume(b)
        dt = time.perf_counter() - t0
        print(f"    snapshot.get_many(batch)      {fmt_rate(count, dt)} lookups/s  {dt / count * 1e6:7.1f} us each")

        t0 = time.perf_counter()
        snap.prefetch(batches[0])
        for i, keys in enumerate(batches):
            if i + 1 < len(batches):
                snap.prefetch(batches[i + 1])
            for b in snap.get_many(keys):
                consume(b)
        dt = time.perf_counter() - t0
        print(f"    get_many + prefetch(next)     {fmt_rate(count, dt)} lookups/s  {dt / count * 1e6:7.1f} us each")

    del blobs
    ro.close()
    return touched


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=None, help="where to put the store and archives (default: a temp dir)")
    ap.add_argument("--scale", type=float, default=1.0, help="multiply every member count by this")
    ap.add_argument("--batch", type=int, default=256)
    args = ap.parse_args()

    root = args.dir or tempfile.mkdtemp(prefix="libsql-bench-")
    os.makedirs(root, exist_ok=True)
    path = os.path.join(root, "bench.db")
    for f in os.listdir(root):
        os.remove(os.path.join(root, f))

    shapes = [("small", 20000, 4 * 1024), ("medium", 2000, 64 * 1024), ("large", 100, 1024 * 1024)]
    shapes = [(n, int(c * args.scale), s) for n, c, s in shapes]

    print("ingest (Writer, one index commit per size class):")
    for name, count, size in shapes:
        ingest(path, name + "_unbuffered", count, size, write_buffer=0)
        ingest(path, name, count, size, write_buffer=4 << 20)

    print("\nreads (read-only reopen, access='random'):")
    for name, count, size in shapes:
        reads(path, name, count, size, args.batch)

    total = sum(os.path.getsize(os.path.join(root, f)) for f in os.listdir(root))
    print(f"\n{total / 2**20:,.0f} MiB on disk under {root}")
    if not args.dir:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
