#!/usr/bin/env python3
"""Bulk payloads beside the tables: libsql.BlobStorage.

Rows describe things; the bytes of those things -- images, tensors, documents
-- live in a tar archive next to the store, indexed by a sub-database inside
it. Reads are zero-copy views into a mapping of that archive, writes are
coalesced into large appends, and a batch of keys is resolved in one pass.

    PYTHONPATH=build/python python3 python/examples/blobs.py
"""

import os
import subprocess
import tempfile

import numpy as np

import libsql

root = tempfile.mkdtemp(prefix="libsql-blobs-")
path = os.path.join(root, "images.db")

db = libsql.Database(path)
# "images" names the index sub-database; the archive defaults to images.tar
# beside the store. access="random" tells the OS not to read ahead, which is
# what you want when lookups land all over the archive.
blobs = libsql.BlobStorage(db, "images", access="random")
print("archive:", blobs.archive_path)

# --- one at a time --------------------------------------------------------
# Keys are bytes, str or int (8 big-endian bytes, so numeric and byte order
# agree). Data is bytes, str, or any C-contiguous buffer -- a numpy array goes
# in without a copy. The tar member name defaults to something derived from
# the key; pass name= to choose.
blobs.put("logo.png", b"\x89PNG\r\n\x1a\n" + bytes(4096))
blobs.put(42, np.arange(12, dtype=np.float32).reshape(3, 4))
blobs.put(b"\x00\x01", b"raw bytes key", name="pair.bin")
print(len(blobs), "members:", [(name, size) for name, _, size in blobs.list()])

# --- reading, without copying ---------------------------------------------
b = blobs["logo.png"]  # KeyError when absent; get() returns a default instead
print(b, "offset", b.offset, "checksum ok:", b.verify())
print("first bytes:", bytes(b)[:8])  # bytes(...) copies; memoryview(b) does not

arr = blobs[42].numpy("float32", (3, 4))  # a read-only view into the archive
print("array view:\n", arr, "\nwritable:", arr.flags.writeable, "base:", type(arr.base).__name__)
# The view keeps its Blob, and so the mapping, alive; copy before mutating.
owned = arr.copy()
owned[0, 0] = -1

# --- many at a time ---------------------------------------------------------
# A Writer queues members in a 4 MiB buffer and commits the index once. This
# is the ingestion path: a million small blobs cost about a thousand writes
# and one index commit, not a million of each.
rng = np.random.default_rng(0)
with blobs.writer() as w:
    for i in range(1000):
        w.add(1000 + i, rng.integers(0, 256, size=2048 + i, dtype=np.uint8))
print(len(blobs), "members after the batch; archive is", blobs.archive_size >> 10, "KiB")

# A batch of keys resolves in one pass over the index, in input order, None
# where a key is absent. Use a Snapshot for a loop of such batches: one read
# transaction instead of one per call.
ids = [1999, 1003, 1500, 7, 1000]
with blobs.snapshot() as snap:
    batch = snap.get_many(ids)
    print("batch sizes:", [None if x is None else len(x) for x in batch])
    # Ask the OS to start reading the *next* batch while this one is consumed.
    snap.prefetch([1100 + i for i in range(64)])
    # Int keys are big-endian, so a byte prefix is a numeric range: 6 zero bytes + 0x03 covers 768..1023.
    print("keys in 768..1023:", len(snap.keys(prefix=(0).to_bytes(6, "big") + b"\x03")))

# --- the archive is a tar file --------------------------------------------
# finalize() terminates and pads it the way tar(1) expects. Anything you
# `tar --append` from a shell is adopted by catch_up() (or on the next open).
blobs.finalize()
try:
    listing = subprocess.run(["tar", "tvf", blobs.archive_path], capture_output=True, text=True, check=True)
    print("tar tvf:", listing.stdout.splitlines()[0], "...")
except (FileNotFoundError, subprocess.CalledProcessError):
    pass

# Erasing drops the index entry only; the bytes stay until the archive is
# repacked. A rebuild from the headers brings everything back -- with the
# default naming undone by key_of="int" (int keys) or "hex" (bytes keys).
blobs.erase(42)
print("42 present:", 42 in blobs, "| members in archive:", len(blobs.list()))
blobs.rebuild_index(key_of="int")
print("42 present after rebuild:", 42 in blobs, "| checksums known now:", blobs[42].has_checksum)

del blobs
db.close()

# --- reopening read-only ---------------------------------------------------
# Any number of processes may open a store read-only at once (a shared lock),
# which is how training workers share one dataset. See nn_dataset.py.
ro = libsql.Database(path, read_only=True)
blobs = libsql.BlobStorage(ro, "images")  # read_only follows the database
print("read-only reopen sees", len(blobs), "members; first array again:", blobs[42].numpy("float32")[:3])
del blobs
ro.close()
print("files:", sorted(os.listdir(root)))
