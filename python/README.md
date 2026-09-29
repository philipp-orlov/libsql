# libsql for Python

> Experimental: this binding has not been through the security/robustness
> review applied to the C++ library and is not built by default.

The SQL engine and the side-car blob archive, for Python. The binding is a
thin pybind11 layer: statements run in the engine, blobs are handed out as
zero-copy views into the archive, and every call that touches the store
releases the GIL.

```python
import libsql, numpy as np

db = libsql.Database("dataset.db")
db.exec("CREATE TABLE sample (id INTEGER PRIMARY KEY, split TEXT, label INTEGER)")
blobs = libsql.BlobStorage(db, "tensors", access="random")

# rows and tensors commit together
with db.begin() as t, blobs.writer(t) as w:
    for i, (x, label) in enumerate(corpus):                 # x: a C-contiguous numpy array
        t.exec("INSERT INTO sample VALUES (?, ?, ?)", (i, "train", label))
        w.add(i, x)                                          # no copy on the way in

# a training step: which ids, then their bytes, then the next batch's pages
ids = [r[0] for r in db.exec("SELECT id FROM sample WHERE split = 'train'")]
with blobs.snapshot() as snap:
    for batch, following in zip(batches(ids), batches(ids)[1:]):
        snap.prefetch(following)                             # I/O for the next step starts now
        x = np.stack([b.numpy("float32", (3, 32, 32)) for b in snap.get_many(batch)])
```

[examples/](examples/) has the full tour: `quickstart.py` (SQL),
`blobs.py` (the archive), `nn_dataset.py` (a fork-safe PyTorch-compatible
dataset with metadata in SQL and tensors in the archive) and
`bench_blobs.py` (ingest and random-read throughput).

## Building

The binding needs the Python development headers (`python3-dev` on
Debian/Ubuntu) and pybind11, which CMake finds installed, inside a
`pip install pybind11`, or fetches (v3.1.0) when neither is there.

```sh
# as part of libsql
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSQL_BUILD_PYTHON=ON
cmake --build build --parallel
PYTHONPATH=build/python python3 -c "import libsql; print(libsql.__version__)"

# or as a wheel, from the libsql checkout
pip install ./python
```

`-DPython_EXECUTABLE=...` picks the interpreter when several are installed.
The build tree is importable as-is: `build/python/libsql` holds the module and
the package. Tests: `PYTHONPATH=build/python python3 -m unittest discover -s python/tests`.

The distribution is called `libsql` and so is the import; note that PyPI's
`libsql` is an unrelated project (Turso's), so install this one from the
checkout, not by name.

## Packaging for other machines (manylinux wheels, x86_64 + aarch64)

The `python/` directory is normally built against the sibling `../libsql` and
`../../libnosql` checkouts, which is fine on a machine that has the whole
source tree. To ship something a *different* machine can `pip install`
without a compiler or those checkouts -- e.g. a training box with no
toolchain, or an architecture you don't develop on -- build a wheel:

```sh
cd python
./vendor.sh          # copies the engine sources into python/_vendor (self-contained)
./build-wheels.sh     # -> python/wheelhouse/*.whl, Python 3.12 only, this host's architecture
```

`build-wheels.sh` runs [cibuildwheel](https://cibuildwheel.pypa.io/) against the
`manylinux_2_28` container images, so the wheels it produces work on any
reasonably current glibc-based Linux distribution regardless of what's
installed there -- no system Python headers, no libnosql checkout, nothing
beyond `pip install` on the target machine. Only the engine's own sources are
vendored (~700 KiB); there are no other third-party C/C++ dependencies to
bundle (`libnosql` links nothing but `Threads`).

A Linux Docker host builds the architecture it natively runs on: an aarch64
box (e.g. an ARM server or single-board computer) produces `aarch64`
wheels, an x86_64 box produces `x86_64` wheels. To get both architectures out
of one host, either:

- run `./build-wheels.sh` once per architecture, on one machine of each, or
- run `./build-wheels.sh --also-emulate`, which registers QEMU binfmt
  handlers (`docker run --privileged tonistiigi/binfmt`) and cross-compiles
  the other architecture under emulation. This works but is noticeably slower
  than a native build, since every compiler invocation runs emulated.

By default only Python 3.12 is built (`build = "cp312-*"` in `pyproject.toml`'s
`[tool.cibuildwheel]`), which is what's needed for arm + intel training boxes
running 3.12. Pass `--all-versions` to build every supported interpreter
instead (cp39-cp313):

```sh
./build-wheels.sh --all-versions                  # all versions, native arch
./build-wheels.sh --all-versions --also-emulate    # all versions, both arches
```

`--all-versions` sets `CIBW_BUILD` (which overrides `pyproject.toml`); to
build one specific other version instead, set `CIBW_BUILD=cp3XX-*` yourself
before calling `./build-wheels.sh`.

Install the result on the target machine with (for example)
`pip install libsql-0.1.0-cp312-cp312-manylinux_2_28_aarch64.whl`, or point
`pip install` at the whole `wheelhouse/` directory to let it pick the file
matching the target's Python version and architecture.

## SQL

`Database(path, *, read_only=False, create=True, durable=True, max_size=64 GiB)`
opens a store; it is a context manager, and `close()` releases the file once
nothing built on it (statements, transactions, blob storages) is left.

`db.exec(sql, params=None)` runs every `;`-separated statement in one
transaction and returns a `Result` for the last: `len()`, iteration and
indexing give `Row`s, `.columns` the names, `.rows` plain tuples, `.changes`
and `.last_insert_id` the counters, `.first()` and `.scalar()` the usual
shortcuts. A `Row` indexes by position or by name (case-insensitive, like the
engine), and has `.keys()`, `.values()`, `.get()` and `.as_dict()`.

Parameters bind left to right from any sequence. Python types map onto the six
storage classes:

| Python | SQL |
|---|---|
| `None` | NULL |
| `bool`, `int`, numpy integer scalars | INTEGER |
| `float`, numpy float scalars | REAL |
| `str` | TEXT |
| `bytes`, `bytearray`, `memoryview`, numpy arrays (C-contiguous) | BLOB |
| `datetime.datetime` (naive is UTC), `datetime.date` | DATETIME |

DATETIME comes back as a timezone-aware `datetime` in UTC. TEXT that is not
valid UTF-8 decodes with `surrogateescape`.

`db.begin()` returns a write `Transaction`, `db.begin_read()` a snapshot that
rejects writes. Both are context managers that commit on a clean exit and roll
back on an exception, and both have `exec`, `commit`, `rollback` and `active`.
Only one write transaction is live at a time; `begin()` blocks for it.

`db.prepare(sql)` returns a `Statement`: `.bind(i, value)` (1-based),
`.bind_all(params)`, `.clear()`, and `.exec(params=None, txn=None)`, which runs
in a transaction of its own or inside `txn`. `.parameters` and `.writes` say
what it takes and whether it needs a writer.

`db.tables()`, `db.table(name)` and `db.indexes(table=None)` describe the schema
as lists and dicts. A table's `"primary_key"` is the list of its key column
names in key order: one name for a plain key, several for a composite one, and
empty for a table keyed by its implicit `rowid`.

Errors are `libsql.SqlError` (from the SQL layer) and `libsql.StoreError` (from
libnosql), both `libsql.Error`, each with `.code` naming the engine's error
code: `"NoSuchTable"`, `"ConstraintViolation"`, `"ReadOnly"`, `"Busy"`, ...

## Blobs

`BlobStorage(db, name="blobs", *, directory=None, file_name=None,
read_only=None, sync_on_append=False, catch_up_on_open=True, access="normal",
write_buffer=4 MiB)` binds a tar archive -- `<name>.tar` beside the store,
unless `directory`/`file_name` say otherwise -- to an index kept in
sub-database `name` of the same store. `read_only` follows the database by
default. The archive is a plain tar file: `tar tvf` lists it, `tar --append`
adds to it and `catch_up()` (also run on open) indexes what arrived.

**Keys** are `bytes`, `str` (UTF-8) or a non-negative `int` (8 big-endian
bytes, so numeric order is byte order). **Data** is `bytes`, `str`, or any
C-contiguous buffer -- a numpy array goes in without a copy. Every member has a
tar **name** (1..100 bytes); when you do not pass one it is the `str` key
itself, sixteen hex digits for an `int`, hex for `bytes`. `rebuild_index()`
reconstructs the index from those names; `key_of="int"` or `"hex"` undo the
default naming, a callable maps names to keys however you like.

Writing:

- `put(key, data, name=None)` appends one member and commits.
- `writer(txn=None)` returns a `Writer` that queues members in a buffer and
  commits the index once. `add(key, data, name=None)`, `commit()`, `abandon()`;
  as a context manager it commits on a clean exit. With `txn`, a `Database.begin()`
  transaction, the index entries go into *that* transaction, so blobs and the
  rows describing them land under one commit or not at all. Abandoning (or
  rolling `txn` back) leaves the bytes in the archive for `catch_up()`.
- `erase(key)` drops the index entry only; `finalize()` terminates and pads
  the archive for tar(1); `sync()` fsyncs it.

Reading -- `Blob` is a view, not a copy:

- `blobs[key]` (KeyError when absent), `get(key, default=None)`, `key in blobs`,
  `len(blobs)`, `keys(prefix=None)`.
- `get_many(keys)` resolves a whole batch in one pass over the index, in input
  order, `None` where a key is absent.
- `prefetch(keys)` asks the OS to start reading those payloads and returns at
  once; `warm()` does the same for the whole archive.
- `snapshot(txn=None)` returns a `Snapshot` with the same read methods against
  one read transaction -- the form for a loop, where a transaction per lookup
  would dominate. With `txn`, a `Database.begin_read()` transaction, SQL rows
  and blobs are read from the same instant.

A `Blob` exports the buffer protocol, read-only, over the archive mapping it
pins: `memoryview(b)`, `np.frombuffer(b, dtype)`, `torch.frombuffer(b, dtype=...)`
see the bytes where they lie, and `b.numpy(dtype, shape)` is the numpy view
reshaped. `bytes(b)` and `b.tobytes()` copy. `.name`, `.size`, `.offset`
(512-aligned, for `os.pread` or `sendfile`), `.checksum`, `.has_checksum` and
`verify()` round it out. The view stays valid for as long as the Blob (or an
array whose `.base` is the Blob) exists, whatever happens to the storage
handle or the archive afterwards; copy it before writing into it.

## Feeding a training loop

The archive is shaped for "write once, read at random forever":

- **Ingest in batches** through a `Writer`, ideally joined to the transaction
  that inserts the rows. The buffer turns a million 4 KiB appends into a
  thousand writes and one index commit. `Database(..., durable=False)` skips
  the fsync per commit while loading.
- **Look up by batch.** `snapshot.get_many(ids)` probes the index in key order,
  so a shuffled batch touches each index leaf once; `Blob.numpy` then gives
  each sample without a copy, and `np.stack` makes the one copy a batch needs.
- **Prefetch the next batch** with `snapshot.prefetch(next_ids)` while the
  current one is on the GPU. On a corpus larger than RAM this turns a blocking
  page fault per sample into I/O that overlaps compute. `access="random"` stops
  the kernel from reading 128 KiB around every 4 KiB sample; `warm()` streams
  a corpus that fits in RAM in on the first epoch.
- **Workers open their own handles.** Any number of processes may open a store
  read-only at once (a shared lock) as long as no writer holds it. A
  `torch.utils.data.Dataset` should open its `Database`, `BlobStorage` and
  `Snapshot` lazily in the process that reads and drop them in `__getstate__`
  -- `examples/nn_dataset.py` does exactly that, and implements
  `__getitems__` so DataLoader fetches a batch in one call.
- **Metadata stays queryable.** Splits, labels, class balance, provenance:
  a `SELECT` answers them, and an index on the column you filter by keeps it
  fast. Keep the tensor's dtype and shape in the row (or fixed by convention)
  and pass them to `Blob.numpy`.

Two things to keep in mind. A `Snapshot` pins a read snapshot for as long as
it lives, which holds back space reclamation while a writer is busy -- hold it
for an epoch, not forever. And a `Writer` holds the store's single write slot,
so do not open a second `Database.begin()` on the same thread while one is
live without joining them (`blobs.writer(txn)`).

## Threads and processes

Reads from any number of threads are fine and run without the GIL. Writes are
serialised by the store. A `Database`, `BlobStorage`, `Snapshot` or `Writer`
must not be used from two threads at once for writes, and none of them survive
a `fork()` -- open them in the child, as the dataset example does.
