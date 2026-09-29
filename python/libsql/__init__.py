"""libsql for Python: a small SQL engine over libnosql, plus a zero-copy blob
archive for the bulk payloads that do not belong in a B+tree.

    import libsql

    db = libsql.Database("catalog.db")
    db.exec("CREATE TABLE sample (id INTEGER PRIMARY KEY, label INTEGER, shape TEXT)")
    blobs = libsql.BlobStorage(db, "tensors", access="random")

    with db.begin() as t, blobs.writer(t) as w:      # rows and bytes commit together
        for i, (arr, label) in enumerate(corpus):
            t.exec("INSERT INTO sample VALUES (?, ?, ?)", (i, label, str(arr.shape)))
            w.add(i, arr)                             # any C-contiguous buffer, zero-copy in

    ids = [r[0] for r in db.exec("SELECT id FROM sample WHERE label = 3")]
    with blobs.snapshot() as snap:
        batch = snap.get_many(ids[:64])               # one pass over the index
        x = numpy.stack([b.numpy("float32", (3, 32, 32)) for b in batch])

See README.md beside this package for the full tour.
"""

from ._libsql import (  # noqa: F401
    Blob,
    BlobStorage,
    Database,
    Error,
    Result,
    Row,
    Snapshot,
    SqlError,
    Statement,
    StoreError,
    Transaction,
    Writer,
    __version__,
)

__all__ = [
    "Blob",
    "BlobStorage",
    "Database",
    "Error",
    "Result",
    "Row",
    "Snapshot",
    "SqlError",
    "Statement",
    "StoreError",
    "Transaction",
    "Writer",
    "__version__",
]


def _blob_numpy(self, dtype="uint8", shape=None):
    """A numpy view of the payload, without copying.

    ``dtype`` is anything ``numpy.dtype`` accepts; ``shape`` reshapes the flat
    view. The array is read-only and its ``base`` keeps this Blob -- and so the
    archive mapping -- alive. Copy it (``.copy()``, ``numpy.stack``) before
    writing into it or handing it to something that wants a writable buffer.
    """
    import numpy as np

    a = np.frombuffer(self, dtype=dtype)
    return a.reshape(shape) if shape is not None else a


Blob.numpy = _blob_numpy
