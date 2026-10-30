# libsql Python binding and training-data blob storage: design

This document is the complete design of the Python binding for libsql and of
the BlobStorage changes made for neural-network training workloads. It is
written so that a person or an agent could rebuild everything from it without
reading the code first. It records what exists, why each choice was made,
which alternatives were rejected, and where the sharp edges are.

Paths are relative to the `libsql` checkout unless stated. libnosql is the
sibling checkout at `../libnosql`.

---

## 1. Problem statement

The user trains neural networks and wants one file-based store that holds

- the **metadata** of a corpus (which sample is in which split, its label,
  its provenance, its tensor's dtype and shape) in a form that can be
  queried, and
- the **payloads** (tensors, images) in a form that a training loop can read
  at random, by the batch, at memory-copy speed, from several worker
  processes at once.

libsql already provided the first as a C++ library: a SQL engine whose tables
live in a libnosql store. libnosql already provided a `BlobStorage` layer that
keeps bulk payloads in a tar archive beside the store, indexed by a
sub-database. What was missing:

1. a Python binding for both,
2. a way to use the two on the *same* store (libsql hid its `nosql::Env`),
3. BlobStorage operations shaped for a training loop: batch lookup,
   prefetch, high-throughput ingest of many small members, and atomic
   "row + blob" writes.

Constraints inherited from libnosql (see `../libnosql/docs/limitations.md`):

- single writer *process*; any number of read-only processes may share a
  store (`flock(LOCK_SH)`) as long as no writer holds `LOCK_EX`;
- the whole store file is memory-mapped; values are borrowed slices valid
  for the transaction;
- no multi-process reader table, so a writer must not be active while other
  processes read.

Constraints of the environment the work was done in (matter for building,
not for design): no `python3-dev`, no passwordless sudo, pip refuses
`--user` installs (PEP 668). Nothing was installed into the system Python.

---

## 2. Architecture overview

```
                 Python process
 ┌────────────────────────────────────────────────────────────┐
 │  import libsql            (python/libsql/__init__.py)      │
 │    re-exports _libsql; adds Blob.numpy()                   │
 │                                                            │
 │  libsql._libsql           (python/src/module.cpp, pybind11)│
 │    Database ─ Transaction ─ Statement ─ Result/Row         │
 │    BlobStorage ─ Writer ─ Snapshot ─ Blob (buffer protocol)│
 └───────────┬────────────────────────────┬───────────────────┘
             │ sql::Database::env()       │ nosql::BlobStorage
             ▼                            ▼
 ┌──────────────────────┐   ┌──────────────────────────────────┐
 │ libsql (C++17)       │   │ libnosql BlobStorage             │
 │ tables, indexes,     │   │ index sub-db in the same store,  │
 │ catalog in sub-dbs   │   │ payloads in <name>.tar beside it │
 └──────────┬───────────┘   └──────────────┬───────────────────┘
            └──────────────┬───────────────┘
                           ▼
              libnosql Env: one store file, mmap, MVCC
```

Two files on disk per dataset: `dataset.db` (store: catalog, tables,
indexes, blob index, blob meta) and `<name>.tar` (payloads; a real tar
archive readable by `tar tvf`). Optionally the archive is placed on another
mount (`directory=`).

Layering rule: the Python module contains **no storage logic**. Anything that
touches bytes, transactions or the file lives in C++ in libnosql/libsql so
that the C++ API is equally capable and the binding is a thin, testable
adapter. Everything added for Python was added to the C++ libraries first,
with C++ tests, then exposed.

---

## 3. Changes to libnosql

### 3.1 `os::advise` (paging hints)

Files: `include/nosql/internal/os.hpp`, `src/internal/os.cpp`.

```cpp
enum class Advice { Normal, Random, Sequential, WillNeed };
void advise(const Mapping&, std::size_t offset, std::size_t length, Advice) noexcept;
```

POSIX: `madvise` with `MADV_NORMAL / MADV_RANDOM / MADV_SEQUENTIAL /
MADV_WILLNEED`. The start is aligned *down* to the system page and the range
is clamped to the mapping (same widening rule as the existing `flushRange`).
Return value ignored: the hints are advisory and never affect correctness.

Windows: only `WillNeed` is implemented, through `PrefetchVirtualMemory`
resolved at run time with `GetProcAddress` (Windows 8+), so older SDKs and
systems build and run without it. The struct
`WIN32_MEMORY_RANGE_ENTRY` is re-declared locally to avoid depending on the
SDK version. **Not compiled or tested in this work** (Linux-only machine).

Why madvise and not `readahead(2)`/`posix_fadvise`: the payloads are read
through the mapping, so hints must be on the mapping to affect the pages the
reader will fault on. `MADV_WILLNEED` on a file-backed mapping triggers
asynchronous read-ahead of exactly that range and returns immediately, which
is what "start the next batch's I/O now" needs. `MADV_POPULATE_READ` was
rejected: it is synchronous (blocks until pages are present) and Linux
5.14+ only.

### 3.2 BlobStorage header additions

File: `include/nosql/blob_storage.hpp`. Existing API is unchanged; these were
added.

| addition | purpose |
|---|---|
| `enum class Access { Normal, Random, Sequential }` | paging hint applied to every archive mapping |
| `Options::accessPattern(Access)` | default `Normal` |
| `Options::writeBuffer(std::size_t)` | Writer coalescing buffer, default 4 MiB, 0 = write through |
| `Writer beginWrite(Txn& txn)` | a Writer that records index entries in a caller's write transaction |
| `std::size_t findMany(Txn&, const Slice* keys, std::size_t n, Blob* out) const` | batch lookup, input order preserved, returns hit count |
| `std::vector<Blob> findMany(Txn&, const std::vector<Slice>&) const` | convenience |
| `std::vector<Blob> findMany(const std::vector<Slice>&) const` | own snapshot |
| `void prefetch(const Blob*, std::size_t) const` | WILLNEED on the payload ranges of already-resolved blobs |
| `std::size_t prefetch(Txn&, const Slice* keys, std::size_t) const` | lookup + prefetch, returns hit count |
| `void warm() const` | WILLNEED over the whole archive |
| `std::uint64_t forEachKey(Txn&, Slice prefix, const std::function<bool(Slice)>&) const` | ordered key walk, optional prefix, stop on `false` |
| `BlobStorage() noexcept;` `Writer() noexcept;` | **moved out of line** (were `= default` inline) |

The last row is a header fix, not a feature. An inline defaulted constructor
of a class holding `std::unique_ptr<Incomplete>` makes GCC instantiate the
`unique_ptr` destructor at every default-construction site (for exception
cleanup), which fails because `Impl`/`State` are only defined in the `.cpp`.
Nothing in libnosql default-constructs these types, so it had gone
unnoticed; the Python wrapper does. Declaring the constructors out of line
(defined `= default` in the `.cpp`) fixes it for every user of the header.

`Blob` is unchanged. An inline fixed-size name buffer (to avoid the
`std::string` allocation per lookup) was considered and rejected: names from
foreign tars can be up to 256 bytes (ustar prefix + name), the allocation is
a small fraction of a B+tree probe, and `std::string` SSO already covers
short names.

### 3.3 BlobStorage implementation

File: `src/blob_storage.cpp`. Structure of `Impl` after the change:

```cpp
struct BlobStorage::Impl {
    Env* env; std::string indexDb, metaDb; std::filesystem::path path;
    os::FileHandle fd; bool readOnly, syncOnAppend;
    Access access; std::size_t writeBuffer;
    bool haveIndex = true, haveMeta = true;     // see 3.3.4
    std::uint64_t appendAt;                      // logical end of last member
    std::shared_ptr<const TarMapping> map;       // atomic load/store
    std::mutex remapMtx, appendMtx;
    Db index(Txn&) const; Db meta(Txn&) const;
    bool indexFor(Txn&, Db& out) const;          // false when a RO opener found no index
    std::shared_ptr<const TarMapping> mapped(std::uint64_t need);   // applies accessPattern
    Blob blobFrom(Slice record);                 // decode index record + pin mapping
    void writeMarker(); std::uint64_t scan(...); 
    std::uint64_t readIndexedUpTo(Txn&) const; void writeIndexedUpTo(Txn&, std::uint64_t) const;
    std::uint64_t adopt(Txn&, std::uint64_t from, keyOf);
    std::uint64_t catchUpIn(Txn&);               // catchUp body, for reuse inside open()
};
```

#### 3.3.1 Buffered Writer

```cpp
struct BlobStorage::Writer::State {
    Impl* impl; Txn owned; Txn* txn;   // txn == &owned, or the caller's transaction
    Db index;                          // resolved once per batch
    std::vector<std::byte> buf; std::size_t capacity; std::uint64_t bufStart;
    bool wrote;
    ~State() { try { flush(); } catch (...) {} }   // see below
    void flush();                      // one pwrite of buf at bufStart, then clear
    void queue(const void*, std::size_t);
};
constexpr std::size_t kDirectDivisor = 32;
```

`add(key, name, contents)`:

1. validate name (1..100 bytes), build the 512-byte ustar header (mtime =
   wall clock, as before);
2. `total = 512 + size + padding` (padding to 512);
3. under `appendMtx`: `at = appendAt`;
   - if `total >= capacity / kDirectDivisor` (128 KiB with the 4 MiB default):
     `flush()` whatever is queued ahead, then `writeAtV(fd, at, {header,
     payload, pad})` — one gathered write, no copy of the payload;
   - else: if the buffer cannot take `total`, `flush()`; if the buffer is
     empty set `bufStart = at`; append header, payload, padding to `buf`;
   - `appendAt = at + total`;
4. XXH64 of the payload; index record written **in place** through
   `Db::reserve(key, 32 + name.size())` + `encodeIndexInto` (no temporary
   `std::string` per member; the old `encodeIndex` still exists for
   `adopt`).

`commit()`: if anything was written: `flush()`, `writeMarker()` (two zero
blocks + truncate to `appendAt + 1024`), optional `fsync` (`syncOnAppend`),
`writeIndexedUpTo(*txn, appendAt)`. Then **commit `owned` only if the writer
owns its transaction**; a joined transaction is left to the caller. Idempotent.

Why the flush lives in `State::~State` and not `Writer::~Writer`: the
documented semantics are "an abandoned Writer leaves its bytes in the
archive, unindexed, for `catchUp()`", and the existing test relies on it.
The Python wrapper abandons a Writer by *move-assigning* an empty one over it
(`w = Writer()`), which destroys `State` through `unique_ptr` assignment
without running `~Writer`. Putting the flush in `~State` covers both scope
exit and overwrite. There is a C++ test for the overwrite path.

Why 1/32 and not 1/2 for the bypass threshold: a `pwrite` syscall costs a
few microseconds; copying 128 KiB costs about the same at ~20 GB/s. Above
that size the copy is a loss. Measured on the development machine (aarch64,
warm cache): 4 KiB members 284k → 563k members/s with the buffer; 64 KiB
neutral; 1 MiB at parity once the threshold was lowered (it had been 15%
slower at 1/2).

Consequences to know about:

- `archiveSize()` (== `appendAt`) is the *logical* end and may run ahead of
  what `list()`/`scan()` see on disk while a Writer holds unflushed bytes.
  Readers cannot observe those members anyway: the index that points at
  them is committed only after `flush()`.
- `finalize()` during a live Writer pads from `appendAt`; the later flush
  fills the gap. Harmless, but call `finalize()` after `commit()`.
- `writeBuffer(0)` restores the old one-write-per-member behaviour exactly.

#### 3.3.2 `beginWrite(Txn&)`

Validates: not read-only storage, `txn.valid()`, `!txn.isReadOnly()`,
`txn.belongsTo(env)`. `State.txn` points at the caller's transaction,
`State.owned` stays empty. The index `Db` handle is resolved from that
transaction once.

Failure modes, documented rather than prevented in C++ (the Python wrapper
adds guards, see 4.6):

- caller commits `txn` before `Writer::commit()`: the `Db` handle raises
  `BadTransaction` on the next `add`; `commit()` would dereference a dangling
  `Txn&` — the header says to commit the Writer first;
- caller aborts `txn` after `Writer::commit()`: index entries vanish, bytes
  stay, `catchUp()` adopts them under their member names. This is the same
  state as an abandoned Writer, by design.

#### 3.3.3 `findMany`

```cpp
Db db; if (!impl_->indexFor(txn, db)) { fill out with Blob(); return 0; }
order = iota(count); if (count > 1) sort order by keys[i].compare(keys[j]);
for i in order: v = db.get(keys[i]); out[i] = v ? blobFrom(*v) : Blob();
```

Probing in key order means a shuffled batch visits each index leaf once
rather than re-descending from the root for neighbouring keys in random
order. Results are placed at the input positions so the caller's batch order
is preserved. Sorting 256 keys is negligible next to 256 probes. A single
cursor with `seek` was considered; `Db::get` per key is simpler and the
probe is cached-page cheap once the leaf is hot.

#### 3.3.4 Fresh / read-only storage without an index

Old behaviour: `Impl::index(t)` used `DbFlags::Create`, which in a **read**
transaction throws `ReadOnly` if the sub-database does not exist yet. A
fresh storage with no `put` yet (or a read-only opener of a store where the
index was never created) would throw from `find`, `count`, `contains`.

New behaviour:

- **read-write open**: one write transaction at open creates the index and
  meta sub-databases (`(void)index(t); (void)meta(t);`) and, unless
  `catchUpOnOpen(false)`, runs `catchUpIn(t)` in the same transaction. Same
  number of commits at open as before.
- **read-only open**: one read transaction records `haveIndex =
  indexDb.empty() || t.hasDb(indexDb)` and `haveMeta = t.hasDb(metaDb)`.
  Read paths go through `indexFor()`; when false they report "absent"/0.
  `readIndexedUpTo` returns 0 when `!haveMeta`.

A read-only opener cannot see an index appear later (flags are fixed at
open), which is consistent with the process model: a writer cannot coexist
with it.

#### 3.3.5 `prefetch`, `warm`, `forEachKey`

`prefetch(blobs, n)`: collect valid, non-empty blobs; sort by (mapping
pointer, offset); merge runs where the next offset is within 4096 bytes of
the current end (members are 512-aligned with a header between them, so
"adjacent" means within a page); one `advise(WillNeed)` per run. Blobs from
different mappings (a growth happened between lookups) are advised on their
own mapping.

`prefetch(txn, keys, n)`: `findMany` into a temporary vector, then the above.
`warm()`: `advise(WillNeed)` over `[0, appendAt)` of the current mapping.

`forEachKey(txn, prefix, fn)`: iterates `db.all()` or `db.prefix(prefix)`;
stops when `fn` returns false; returns visited count (including the one that
stopped it).

`mapped()` applies `accessPattern` to each new mapping right after `mmap`:
`Random` → `MADV_RANDOM` (no read-ahead: a 4 KiB blob costs 4 KiB of I/O
instead of the 128 KiB default window), `Sequential` → `MADV_SEQUENTIAL`.

Rejected: mapping with geometric headroom beyond EOF to avoid remaps during
interleaved append/read. On Windows `CreateFileMapping` larger than the file
*extends the file*, which would corrupt the tar layout. Training does not
append while reading, so the win was not worth a platform hazard.

### 3.4 Tests added (`tests/test_blob_storage.cpp`)

`freshStorageReadsAsEmpty`, `findManyKeepsInputOrderAndMarksMisses`,
`bufferedWriterCoalescesSmallAndBypassesLarge` (256 KiB buffer so all three
paths are exercised: queued, flushed-because-full, direct; then `tar`
well-formedness), `writeBufferZeroWritesThrough`,
`abandonedBufferedWriterStillReachesTheArchive`,
`writerOverwrittenByMoveAssignmentFlushesToo`, `writerJoinsACallerTransaction`
(commit-together, abort-together, read txn refused),
`prefetchAndAccessHintsChangeNothingVisible`, `forEachKeyWalksInOrderAndByPrefix`,
`readOnlyOpenersShareAStoreNobodyWrites` (two RO `Env`s on one file in one
process — `flock` shared locks on separate descriptors behave as two
processes would), `readOnlyOpenerOfAStoreWithoutAnIndexSeesNothing`.
27 cases total; `test_mq` also exercises BlobStorage and still passes.

### 3.5 README

`../libnosql/README.md` "Blob storage" section documents `findMany`,
`prefetch`, `warm`, `forEachKey`, the buffered Writer, `beginWrite(txn)`, and
the two new options in the table.

---

## 4. Changes to libsql (C++)

### 4.1 Store accessors

`include/sql/sql.hpp` forward-declares `nosql::Env` and `nosql::Txn` (the
public header still does not include libnosql headers) and adds:

```cpp
nosql::Env& Database::env() const;     // InvalidArgument if closed
nosql::Txn& Transaction::txn();        // BadTransaction if finished
```

Both are borrowed references into the pimpl (`DatabaseImpl::env`,
`TxnImpl::txn`). `Database::close()` destroys the `Env` *object* (the
`shared_ptr<DatabaseImpl>` is reset), so anything holding `env()` must be
gone first — this is the reason for the deferred-close scheme in 4.5.

Reserved sub-database names in a libsql store (from
`src/internal/catalog.cpp`): `sql_catalog`, `tbl:<table>`, `idx:<index>`.
A BlobStorage index must not use these; the binding rejects them.

### 4.2 Build option

`CMakeLists.txt`: `option(SQL_BUILD_PYTHON ... OFF)`; when on,
`set(CMAKE_POSITION_INDEPENDENT_CODE ON)` **before** `add_subdirectory` of
libnosql (static libraries linked into a shared module must be PIC; on
aarch64 non-PIC objects fail to link into a `.so`), and
`add_subdirectory(python)` at the end.

### 4.3 Test

`tests/test_blobs.cpp` (registered in `tests/CMakeLists.txt`): rows and blob
index committed together and rolled back together via `beginWrite(t.txn())`;
`findMany(r.txn(), keys)` from a `beginRead()` snapshot; `txn()` after
rollback throws `BadTransaction`; `env()` after `close()` throws
`InvalidArgument`; blob sub-databases invisible to `tables()`.

---

## 5. The Python binding

### 5.1 Layout

```
python/
  CMakeLists.txt        # module target; standalone/pip mode pulls in ..
  pyproject.toml        # scikit-build-core, name "libsql", packages ["libsql"]
  README.md             # user documentation
  docs/design.md        # this file
  src/module.cpp        # the whole binding, one translation unit
  libsql/__init__.py    # re-exports + Blob.numpy
  examples/quickstart.py blobs.py nn_dataset.py bench_blobs.py
  tests/test_libsql.py  # unittest, 18 cases
```

Module name `libsql._libsql`; package `libsql`. pybind11 (v3.1.0 was used;
≥2.12 should work). Build output is assembled at
`<build>/python/libsql/{__init__.py,_libsql*.so}` so the build tree is
importable with `PYTHONPATH=<build>/python`.

Why pybind11 over nanobind: the buffer protocol on a class
(`py::buffer_protocol()` + `def_buffer`) gives zero-copy `memoryview`,
`np.frombuffer` and `torch.frombuffer` with no numpy/torch dependency in
the extension; pybind11 is the more widely known tool. nanobind's lower
per-call overhead would matter little next to a B+tree probe and a page
fault. Rejected also: raw CPython C API (too much code for the same result).

### 5.2 Wrapper structs (C++ side)

pybind11 binds these plain structs, never the libsql/libnosql classes
directly (except `nosql::Blob`), so lifetimes can be managed explicitly.

```cpp
struct Holder {                       // shared by Database and every BlobStorage on it
    sql::Database db; bool readOnly;
    std::mutex mtx; int attached = 0; bool closeRequested = false;
    void requestClose(); void attach(); void detach(); bool closed();
};
struct Database    { std::shared_ptr<Holder> h; sql::Database& db(); /* throws if closed */ };
struct Transaction { std::shared_ptr<Holder> h; sql::Transaction t; bool readOnly; sql::Transaction& live(); };
struct Statement   { sql::Statement s; };
struct Columns     { std::vector<std::string> names; std::unordered_map<std::string,size_t> byLower; };
struct Row         { std::shared_ptr<const Columns> cols; py::tuple values; };
struct Result      { std::shared_ptr<const Columns> cols; py::list rows; uint64_t changes; int64_t lastInsertId; };
struct BlobStorage { std::shared_ptr<Holder> h; nosql::BlobStorage store; bool readOnly; ~BlobStorage(); };
struct Snapshot    { BlobStorage* store; nosql::Txn own; Transaction* borrowed; py::object keep, keepTxn; };
struct Writer      { BlobStorage* store; nosql::BlobStorage::Writer w; Transaction* borrowed; py::object keep, keepTxn; };
```

`nosql::Blob` is bound as `libsql.Blob` directly; it is copyable and pins its
mapping through a `shared_ptr`, which is exactly the ownership the buffer
protocol needs.

### 5.3 Value conversion (`fromPy` / `toPy`)

Python → `sql::Value`, checked in this order (order matters):

1. `None` → NULL
2. `bool` (before int; bool is an int subclass) → INTEGER 0/1
3. `int` → INTEGER (`int64`; `OverflowError` propagates)
4. `float` → REAL (numpy `float64` is a float subclass and lands here)
5. `str` → TEXT (UTF-8)
6. `bytes` → BLOB
7. `datetime.date` or `datetime.datetime` → DATETIME. Naive datetimes are
   taken as UTC; a `date` becomes midnight UTC. Microseconds are computed
   with exact integer arithmetic on the `timedelta` from the epoch
   (`days*86400e6 + seconds*1e6 + microseconds`), never via
   `timestamp()` (a double, which rounds at microsecond resolution).
8. **numpy scalar or 0-d array**: detected as "has `dtype` and `ndim == 0`".
   `dtype.kind` in `b i u` → INTEGER via `PyNumber_Long`; `f` → REAL via
   `PyNumber_Float`. This must precede the buffer check because numpy
   scalars export the buffer protocol and would otherwise become BLOBs
   (this was a real bug found by the tests).
9. anything exporting the buffer protocol (`bytearray`, `memoryview`,
   `ndarray`) → BLOB, **C-contiguous required** (`TypeError` otherwise,
   suggesting `numpy.ascontiguousarray`). Contiguity is verified from the
   `buffer_info` strides.
10. objects with `__index__` → INTEGER; with `__float__` → REAL (Decimal,
    Fraction); else `TypeError`.

`sql::Value` → Python: NULL→`None`, INTEGER→`int`, REAL→`float`,
TEXT→`str` decoded with `surrogateescape` (TEXT is arbitrary bytes in the
engine), BLOB→`bytes` (a copy; SQL blobs are owned `std::vector` values, not
mapped), DATETIME→`datetime.datetime` aware in UTC, built as `epoch +
timedelta(microseconds=usec)` where `epoch` is a process-lifetime cached
handle (`datetime(1970,1,1,tzinfo=utc)`, deliberately leaked so no
destructor runs after interpreter teardown).

`datetime` module objects are fetched with `py::module_::import` per call
(a `sys.modules` lookup); only DATETIME columns pay for it.

`Params`: any sequence; a bare `str`/`bytes` is rejected with `TypeError`
(a classic footgun: `("x")` is a `str`).

### 5.4 Result / Row

Results are materialised eagerly in C++ into Python objects: a shared
`Columns` (names + lowercase→first-index map, mirroring the engine's
case-insensitive column lookup) and one `Row` per row holding a `py::tuple`
of converted values. Rationale: one pass in C++ is faster than lazy
per-access conversion, and a `Row` object with `__getitem__` by name or
position, `keys()`, `values()`, `get()`, `as_dict()`, `__contains__`,
`__iter__`, `__len__`, `__repr__` is the expected ergonomics. `Result` has
`columns`, `rows` (plain tuples), `changes`, `last_insert_id`, `__len__`,
`__getitem__` (negative indices ok), `__iter__`, `first()`, `scalar()`.

No `__bool__` on `Result`: an UPDATE result with zero rows would be falsy,
which misleads.

Pitfall met: `py::keep_alive<0,1>` on `__iter__` fails at run time because
`list_iterator` cannot be weak-referenced. Not needed anyway — the iterator
owns a reference to the list.

Pitfall met: returning a pybind11 *accessor* (`r.values[i]`) from a lambda
compiles but must be wrapped in `py::object(...)`; likewise
`accessor.cast<T&>()` on a temporary trips `-Wdangling-reference`; bind the
`py::object` to a local first.

### 5.5 Lifetimes and close semantics

Problem: `nosql::BlobStorage` borrows `Env&`; `sql::Database::close()`
destroys the `Env`. In Python, objects die in arbitrary order and `close()`
may be called with a `BlobStorage` still alive.

Solution (`Holder`):

- `BlobStorage(db, ...)` calls `h->attach()` and holds the same
  `shared_ptr<Holder>`; `py::keep_alive<1,2>` also keeps the Python
  `Database` object alive while the storage lives.
- `Database.close()` calls `requestClose()`: sets `closeRequested`; if
  `attached == 0` closes the store now, otherwise defers. The Python
  `Database` refuses further work immediately (`closed` is true, methods
  raise `ValueError`). This mirrors libsql's own rule that live statements and
  transactions keep a closed database open.
- `~BlobStorage()` first resets `store` (so the `Env` it borrows is still
  alive), then `detach()`, which performs the deferred close when it was the
  last attachment.
- `Snapshot` and `Writer` hold `py::object keep` (the `BlobStorage` Python
  object) and `keepTxn` (the `Transaction`, when joined) and **drop both on
  `close()`/`commit()`/`abandon()`/`__exit__`**. `py::keep_alive` was used
  first and rejected: it pins the patient until the nurse *dies*, so a
  committed writer left in a variable kept the whole store open (and locked
  against other processes). Explicit references released at the end of the
  object's useful life fix that.
- `Statement` and `Transaction` need nothing extra: libsql's own impls hold a
  `shared_ptr<DatabaseImpl>`.

Blobs: a `Blob` never references the storage or the database, only its
mapping. Views made from it (`memoryview`, `np.frombuffer`) hold the Blob as
their exporter/`base`. So a Blob or array outlives the storage, the database
and any archive growth (growth remaps; old mappings stay alive while
referenced).

### 5.6 Snapshot and Writer specifics

`Snapshot` from `blobs.snapshot()` owns a `nosql::Txn` read transaction.
From `blobs.snapshot(txn)` it *borrows* a libsql `Transaction` and calls
`borrowed->live().txn()` on **every** operation rather than caching the
`Txn&`, because the libsql transaction may commit/rollback (its impl is
reset, the reference would dangle). `live()` raises `SqlError(BadTransaction)`
in that case.

`Writer` from `blobs.writer()` owns its transaction via
`nosql::BlobStorage::beginWrite()`; from `blobs.writer(txn)` it joins via
`beginWrite(t.live().txn())` and rejects read-only transactions with
`ValueError`. `check()` before `add`/`commit` calls `borrowed->live()` so a
transaction finished underneath is reported instead of dereferenced.
`finish(commit)` commits or abandons (move-assign an empty Writer, which
flushes and never indexes), then drops all references. Context manager:
commit on clean exit, abandon on exception.

Both the `Writer` and `Database.begin()` take the store's single write slot;
holding one of each on the same thread without joining deadlocks (documented
in the README).

### 5.7 Keys and member names

`keyBytes(obj)`:

| Python | key bytes |
|---|---|
| `bytes` | as is |
| `str` | UTF-8 |
| `int` ≥ 0, < 2⁶⁴ | 8 bytes big-endian (`PyLong_AsUnsignedLongLong`; `OverflowError` for negatives/too large) |
| other buffer | its bytes |
| else | `TypeError` |

Big-endian ints make byte order equal numeric order: sorted probing in
`findMany` visits neighbours together, and a byte prefix is a numeric range
(`(0).to_bytes(6,"big") + b"\x03"` selects 768..1023).

`defaultName(key)` when `name=` is omitted: `str` key → itself (must be 1..100
bytes, no NUL, else `ValueError` asking for `name=`); `int` → `%016x`;
`bytes` → hex (≤ 50 bytes, else `ValueError`). These are invertible so
`rebuild_index(key_of="int")` / `"hex"` can restore the original keyspace
from member names alone; `key_of` may also be a Python callable (called with
the GIL re-acquired) or `None` (name is the key).

`keys()`/`Snapshot.keys()` return `bytes` (no attempt to guess the original
type).

### 5.8 GIL policy

Every call into the store or archive runs under `py::gil_scoped_release`:
`Database.__init__`, `exec`, `begin`, `begin_read`, `Transaction.exec/commit`,
`Statement.exec`, `BlobStorage.__init__`, `put`, `__len__`, `get`,
`__getitem__`, `__contains__`, `get_many`, `prefetch`, `warm`, `keys`,
`erase`, `finalize`, `sync`, `catch_up`, `rebuild_index`, `list`,
`indexed_up_to`, `Writer.add/commit`, `Snapshot.*`, `Blob.verify`. The
pattern is always: convert arguments with the GIL, release, call, re-acquire,
build results. Buffers passed to `add`/`put` stay valid across the release
because the `py::buffer_info` (a held reference + exported buffer) lives in
the calling frame.

### 5.9 Errors

Exception types created at module init:

- `libsql.Error` — `PyErr_NewException("libsql.Error", Exception)`;
- `libsql.SqlError(Error)` — `py::exception<sql::Error>`;
- `libsql.StoreError(Error)` — `py::exception<nosql::Error>`.

One `register_exception_translator` catches `sql::Error` and `nosql::Error`,
instantiates the type with `what()`, sets `.code` to the **enum identifier**
(`codeName()` switches for both enums: `"NoSuchTable"`, `"Busy"`, ...) and
raises via `PyErr_SetObject`. The engines' own `toString()` return human
phrases ("no such table"), which are not stable identifiers to match on.

Python-native exceptions are used where Python users expect them: `KeyError`
from `blobs[key]`/`snap[key]`/`row["x"]`, `IndexError` from bad positions,
`TypeError`/`ValueError`/`OverflowError` for bad arguments, `ValueError` for
use of a closed object.

### 5.10 Python-side additions (`__init__.py`)

Only `Blob.numpy(dtype="uint8", shape=None)`: `np.frombuffer(self, dtype)`
reshaped, read-only, `base` is the Blob. numpy is imported lazily so the
extension has no numpy dependency. Setting a function attribute on a
pybind11 class works (heap type).

---

## 6. Build system

### 6.1 `python/CMakeLists.txt`

- If it is the top-level project (standalone or pip): sets C++17, PIC,
  Release, turns off libsql tests/examples/tools and `SQL_BUILD_PYTHON`
  (to avoid recursion), then `add_subdirectory(.. <bin>/libsql)`.
- `find_package(Python 3.8 COMPONENTS Interpreter Development.Module REQUIRED)`.
  `Development.Module` (not `Development`) so no `libpython` link is
  required.
- pybind11 discovery, in order: `find_package(pybind11 CONFIG)`;
  `python -m pybind11 --cmakedir` (pip-installed wheel); `FetchContent` of
  tag `v3.1.0` (needs network).
- `pybind11_add_module(_libsql MODULE src/module.cpp)`, links `sql::sql`,
  warnings `-Wall -Wextra -Wshadow` (clean).
- Output directory `<build>/python/libsql`; a `POST_BUILD` step copies
  `libsql/*.py` beside the module.
- `if(SKBUILD) install(TARGETS _libsql LIBRARY DESTINATION libsql)` so the
  wheel gets the module inside the package; not installed for a plain
  `make install`.

### 6.2 `pyproject.toml`

scikit-build-core backend, build requirements `scikit-build-core>=0.10`,
`pybind11>=2.12`; `wheel.packages = ["libsql"]`; `cmake.source-dir` defaults
to `python/`, whose CMakeLists pulls in the parent. `pip install ./python`
was verified end to end in a venv (`--system-site-packages` for numpy),
including running the test suite against the installed wheel.

Distribution name is `libsql`, which collides with Turso's PyPI package of
the same name; this one is only ever installed from the checkout. Noted in
the README.

### 6.3 Building without `python3-dev` (what was actually done here)

Headers came from `apt-get download libpython3.12-dev` + `dpkg -x` into a
scratch directory. `FindPython` was pointed at them with
`-DPython_INCLUDE_DIR=<root>/usr/include/python3.12`; Ubuntu's
`pyconfig.h` includes the multiarch `<aarch64-linux-gnu/python3.12/pyconfig.h>`,
so `-DCMAKE_CXX_FLAGS=-I<root>/usr/include` was also needed. pybind11 came
from the wheel unpacked with `zipfile` (`-Dpybind11_DIR=<...>/share/cmake/pybind11`).
For pip, the same two CMake flags go through the `CMAKE_ARGS` environment
variable. The proper fix is `sudo apt install python3-dev`.

---

## 7. Public Python API (reference)

```
Database(path, *, read_only=False, create=True, durable=True, max_size=64<<30)
  .exec(sql, params=None) -> Result       .prepare(sql) -> Statement
  .begin() -> Transaction                 .begin_read() -> Transaction
  .tables() -> [str]  .table(name) -> dict  .indexes(table=None) -> [dict]
      table(): {"name", "columns": [dict], "primary_key": [str]}  (key columns in order)
  .path .read_only .closed  .close()      context manager (closes)

Transaction: .exec(sql, params=None) .commit() .rollback() .active .read_only
             context manager: commit on success, rollback on exception
Statement:   .sql .parameters .writes  .bind(i, v) .bind_all(params) .clear()
             .exec(params=None, txn=None) -> Result
Result:      .columns .rows .changes .last_insert_id  len/iter/[i]  .first() .scalar()
Row:         [i] / [name] (case-insensitive)  .get(k, default) .keys() .values() .as_dict()

BlobStorage(db, name="blobs", *, directory=None, file_name=None, read_only=None,
            sync_on_append=False, catch_up_on_open=True, access="normal",
            write_buffer=4<<20)
  .put(key, data, name=None)      .writer(txn=None) -> Writer
  [key] / .get(key, default=None) / key in / len()   .get_many(keys) -> [Blob|None]
  .prefetch(keys) -> int   .warm()   .keys(prefix=None) -> [bytes]
  .snapshot(txn=None) -> Snapshot
  .erase(key) -> bool  .finalize()  .sync()  .catch_up() -> int
  .rebuild_index(key_of=None|"int"|"hex"|callable) -> int  .list() -> [(name, offset, size)]
  .archive_path .archive_size .indexed_up_to .read_only

Writer:   .add(key, data, name=None) .commit() .abandon() .active   ctx: commit/abandon
Snapshot: [key] .get() .get_many() .prefetch() .keys(prefix) in .close() .active  ctx
Blob:     buffer protocol (read-only)  len()  .name .size .offset .checksum .has_checksum
          .verify() .tobytes() .numpy(dtype="uint8", shape=None)
Error / SqlError(.code) / StoreError(.code)
```

`access` ∈ {"normal", "random", "sequential"}. Index names `sql_catalog`,
`tbl:*`, `idx:*` and the empty string are rejected (`ValueError`).

---

## 8. Recommended training-loop shape

1. **Ingest**: `Database(path, durable=False)`; per chunk of a few thousand
   samples, `with db.begin() as t, blobs.writer(t) as w:` insert the row and
   `w.add(id, array)`. Chunking bounds the transaction's dirty set (libnosql
   `dirtyLimit`, 256 MiB default). `finalize()` at the end if tar tools
   will read the archive.
2. **Query** ids and labels once with SQL (`SELECT id, label FROM sample
   WHERE split = ?`).
3. **Read**: per process, open `Database(read_only=True)`,
   `BlobStorage(..., access="random")`, one `Snapshot` per epoch. Per batch:
   `snap.prefetch(next_ids)` then `snap.get_many(ids)`, `np.stack([b.numpy(dtype,
   shape) ...])` — the stack is the single copy a batch needs (the collate
   step needs an owned, writable tensor anyway; `torch.frombuffer` on the
   read-only view would warn).
4. **Workers**: the dataset object opens its handles lazily keyed on
   `os.getpid()` and drops them in `__getstate__`, so it survives both
   `fork` and `spawn`. Implement `__getitems__` so DataLoader fetches a batch
   in one `get_many`. Never open a writer while workers read.

`examples/nn_dataset.py` is the reference implementation of this shape.

---

## 9. Testing

- C++: `../libnosql/build/tests/test_tar` (27), `test_mq`; libsql `ctest`
  (9 suites incl. `blobs`; `soak` excluded from quick runs).
- Python: `PYTHONPATH=<build>/python python3 -m unittest discover -s python/tests`
  — 18 cases covering SQL basics, type round-trips (incl. numpy scalars and
  arrays, datetimes), error codes, transactions, prepared statements,
  read-only open, blob put/get zero-copy (address equality across two
  `frombuffer` calls), key kinds, buffered writer + `get_many`, abandoned
  writer + `catch_up`, writer joined to a SQL transaction (commit and
  rollback), snapshot on a SQL read transaction, `rebuild_index` inverses,
  deferred close, options and read-only reopen, and three forked read-only
  workers sharing the store while the parent holds a read-only handle.
- Examples are run as smoke tests; `bench_blobs.py --scale 0.25` gives
  quick numbers.

---

## 10. Measured behaviour (development machine, aarch64, warm page cache)

| workload | before | after |
|---|---|---|
| ingest 4 KiB members | ~284k/s (write-through) | ~563k/s (4 MiB buffer) |
| ingest 64 KiB | ~60k/s | neutral |
| ingest 1 MiB | ~6k/s | parity (bypasses buffer) |
| random `blobs[key]` (own txn) | | ~0.7 M lookups/s |
| random `snap[key]` | | ~1.0 M lookups/s |
| `snap.get_many(128)` | | ~1.2 M lookups/s |

`prefetch` costs a syscall per merged range and shows as pure overhead on a
warm cache; its benefit appears only when pages are cold (corpus larger than
RAM, or after dropping caches).

---

## 11. Known limitations and future work

- Windows `advise()` untested; `Random`/`Sequential` are no-ops there.
- No real deletion of blobs (archive is append-only; `erase` drops the index
  entry). Repacking would be a separate tool.
- `keys()` materialises a Python list; a generator over a cursor would be
  needed for very large indexes.
- `Blob` name allocation per lookup remains (see 3.2 rationale).
- Datetime values before 1970 or beyond `int64` microseconds are not
  special-cased beyond what Python's `datetime` allows.
- `Result` rows are fully materialised; a streaming cursor API would be
  needed for very large SELECTs.
- The tensor's dtype/shape is not stored by BlobStorage; keep it in the SQL
  row (as the example does) or by convention.
