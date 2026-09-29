#!/usr/bin/env python3
"""A training dataset on libsql: metadata in SQL, tensors in the blob archive.

The split of labour is the point. Rows are what you query -- which ids are in
the train split, what their labels are, how many there are per class -- and
the SQL engine answers those. Tensors are what you stream into the model, and
the blob archive hands them out as zero-copy views, in batches, with the next
batch's pages requested from the OS before you need them.

    PYTHONPATH=build/python python3 python/examples/nn_dataset.py [N] [--workers K]

Runs with PyTorch's DataLoader when torch is importable and with a plain
numpy loop otherwise; the dataset class is the same either way.
"""

import argparse
import os
import sys
import tempfile
import time

import numpy as np

import libsql

SHAPE = (3, 32, 32)  # CIFAR-shaped float32 samples: 12 KiB each
DTYPE = "float32"


# --------------------------------------------------------------------------
# Building the dataset
# --------------------------------------------------------------------------
def build(path, n, seed=0):
    """Write n samples. Rows and bytes commit together: the writer joins the
    SQL transaction, so a crash mid-way leaves no row without its tensor and
    no tensor the index knows about without its row."""
    rng = np.random.default_rng(seed)
    db = libsql.Database(path, durable=False)  # bulk load: skip the fsync per commit
    db.exec(
        """
        CREATE TABLE sample (
            id     INTEGER PRIMARY KEY,
            split  TEXT NOT NULL,
            label  INTEGER NOT NULL,
            source TEXT);
        CREATE INDEX sample_by_split ON sample (split);
        """
    )
    blobs = libsql.BlobStorage(db, "tensors")
    insert = db.prepare("INSERT INTO sample (id, split, label, source) VALUES (?, ?, ?, ?)")
    t0 = time.perf_counter()
    batch = 5000  # keep each transaction's dirty set bounded
    for start in range(0, n, batch):
        with db.begin() as t, blobs.writer(t) as w:
            for i in range(start, min(start + batch, n)):
                x = rng.standard_normal(SHAPE, dtype=np.float32)
                label = int(rng.integers(0, 10))
                insert.exec((i, "train" if i % 10 else "val", label, f"synthetic-{i}"), txn=t)
                w.add(i, x)  # int key -> 8 big-endian bytes; the array goes in without a copy
    dt = time.perf_counter() - t0
    mb = blobs.archive_size / 2**20
    print(f"built {n} samples ({mb:.0f} MiB) in {dt:.2f}s: {n / dt:,.0f} samples/s, {mb / dt:,.0f} MiB/s")
    blobs.finalize()
    del blobs
    db.close()


# --------------------------------------------------------------------------
# Reading it back, in whichever process asks
# --------------------------------------------------------------------------
class BlobDataset:
    """A map-style dataset (torch.utils.data.Dataset-compatible) over one split.

    Handles are opened lazily in the process that reads, so the object can be
    created in the parent, pickled or forked into workers, and each worker
    ends up with its own read-only Database, BlobStorage and Snapshot. Any
    number of read-only openers may share the store; none of them writes.
    """

    def __init__(self, path, split):
        self.path = path
        self.split = split
        with libsql.Database(path, read_only=True) as db:
            rows = db.exec("SELECT id, label FROM sample WHERE split = ? ORDER BY id", (split,))
            self.ids = np.array([r[0] for r in rows], dtype=np.int64)
            self.labels = np.array([r[1] for r in rows], dtype=np.int64)
        self._pid = None

    # -- per-process handles ------------------------------------------------
    def _open(self):
        if self._pid != os.getpid():
            self._db = libsql.Database(self.path, read_only=True)
            self._blobs = libsql.BlobStorage(self._db, "tensors", access="random")
            self._snap = self._blobs.snapshot()  # one read transaction for the whole epoch
            self._pid = os.getpid()

    def __getstate__(self):
        state = self.__dict__.copy()
        for k in ("_db", "_blobs", "_snap"):
            state.pop(k, None)
        state["_pid"] = None
        return state

    # -- the Dataset protocol ------------------------------------------------
    def __len__(self):
        return len(self.ids)

    def __getitem__(self, i):
        self._open()
        view = self._snap[int(self.ids[i])].numpy(DTYPE, SHAPE)
        # The view is read-only and pins the archive mapping; the copy is the
        # one and only copy a sample makes on its way into a batch.
        return np.array(view), int(self.labels[i])

    def __getitems__(self, indices):
        """Batched fetch: one pass over the index for the whole batch. Recent
        DataLoaders call this when it exists instead of __getitem__ per item."""
        self._open()
        keys = [int(self.ids[i]) for i in indices]
        blobs = self._snap.get_many(keys)
        return [(np.array(b.numpy(DTYPE, SHAPE)), int(self.labels[i])) for b, i in zip(blobs, indices)]

    def prefetch(self, indices):
        """Start reading these samples' pages now; returns immediately."""
        self._open()
        self._snap.prefetch([int(self.ids[i]) for i in indices])


def run_numpy_loop(ds, batch_size, epochs):
    """No torch: a shuffled epoch, batch by batch, prefetching one batch ahead."""
    rng = np.random.default_rng(1)
    for epoch in range(epochs):
        order = rng.permutation(len(ds))
        batches = [order[i : i + batch_size] for i in range(0, len(order), batch_size)]
        t0 = time.perf_counter()
        seen = 0
        checksum = 0.0
        if batches:
            ds.prefetch(batches[0])
        for k, idx in enumerate(batches):
            if k + 1 < len(batches):
                ds.prefetch(batches[k + 1])  # overlap the next batch's I/O with this one's compute
            samples = ds.__getitems__(idx)
            x = np.stack([s[0] for s in samples])  # (B, 3, 32, 32)
            y = np.array([s[1] for s in samples])
            checksum += float(x.mean()) + float(y.sum())  # stands in for the training step
            seen += len(idx)
        dt = time.perf_counter() - t0
        print(f"epoch {epoch}: {seen} samples in {dt:.2f}s = {seen / dt:,.0f} samples/s "
              f"({seen * np.prod(SHAPE) * 4 / dt / 2**20:,.0f} MiB/s), checksum {checksum:.3f}")


def run_torch_loop(ds, batch_size, epochs, workers):
    import torch
    from torch.utils.data import DataLoader

    loader = DataLoader(ds, batch_size=batch_size, shuffle=True, num_workers=workers,
                        persistent_workers=workers > 0, drop_last=False)
    for epoch in range(epochs):
        t0 = time.perf_counter()
        seen = 0
        checksum = torch.zeros(())
        for x, y in loader:  # x: float32 (B, 3, 32, 32), y: int64 (B,)
            checksum += x.mean() + y.sum()
            seen += x.shape[0]
        dt = time.perf_counter() - t0
        print(f"epoch {epoch}: {seen} samples in {dt:.2f}s = {seen / dt:,.0f} samples/s "
              f"with {workers} workers, checksum {float(checksum):.3f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("n", nargs="?", type=int, default=4000, help="samples to generate (default 4000)")
    ap.add_argument("--workers", type=int, default=2, help="DataLoader workers when torch is present")
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--epochs", type=int, default=2)
    ap.add_argument("--path", default=None, help="reuse a dataset built earlier instead of generating")
    args = ap.parse_args()

    path = args.path or os.path.join(tempfile.mkdtemp(prefix="libsql-nn-"), "dataset.db")
    if not args.path:
        build(path, args.n)

    # The SQL side answers the questions about the corpus...
    with libsql.Database(path, read_only=True) as db:
        print("per split:", db.exec("SELECT split, COUNT(*) AS n FROM sample GROUP BY split").rows)
        print("train label histogram:",
              db.exec("SELECT label, COUNT(*) FROM sample WHERE split = 'train' GROUP BY label").rows)

    # ...and the dataset streams the tensors.
    ds = BlobDataset(path, "train")
    x, y = ds[0]
    print(f"sample 0: {x.shape} {x.dtype} label {y}; {len(ds)} training samples")

    try:
        import torch  # noqa: F401
        have_torch = True
    except ImportError:
        have_torch = False

    if have_torch:
        run_torch_loop(ds, args.batch, args.epochs, args.workers)
    else:
        print("torch not installed; running the numpy loop (same dataset class)")
        run_numpy_loop(ds, args.batch, args.epochs)
    print("dataset at", path)


if __name__ == "__main__":
    sys.exit(main())
