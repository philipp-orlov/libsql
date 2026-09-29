"""Tests for the libsql Python binding.

Run from a build tree:

    PYTHONPATH=build/python python3 -m unittest discover -s python/tests -v

or, after `pip install ./python`, simply `python3 -m unittest discover -s python/tests`.
"""

import datetime as dt
import multiprocessing
import os
import shutil
import tempfile
import unittest

import libsql

try:
    import numpy as np
except ImportError:  # pragma: no cover - numpy is optional for the binding itself
    np = None

UTC = dt.timezone.utc


class Scratch(unittest.TestCase):
    """A temp directory per test, removed afterwards."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="libsql-py-")
        self.path = os.path.join(self.dir, "test.db")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)


class SqlBasics(Scratch):
    def test_create_insert_select(self):
        db = libsql.Database(self.path)
        db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL, price REAL)")
        r = db.exec("INSERT INTO t (name, price) VALUES (?, ?), (?, ?)", ("a", 1.5, "b", 2.5))
        self.assertEqual(r.changes, 2)
        self.assertEqual(r.last_insert_id, 2)
        rows = db.exec("SELECT id, name, price FROM t ORDER BY id")
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows.columns, ["id", "name", "price"])
        self.assertEqual(rows[0]["name"], "a")
        self.assertEqual(rows[0]["NAME"], "a")  # case-insensitive, as in the engine
        self.assertEqual(rows[1][2], 2.5)
        self.assertEqual(rows[-1]["id"], 2)
        self.assertEqual(rows.rows, [(1, "a", 1.5), (2, "b", 2.5)])
        self.assertEqual([tuple(r) for r in rows], rows.rows)
        self.assertEqual(rows[0].as_dict(), {"id": 1, "name": "a", "price": 1.5})
        self.assertEqual(rows[0].get("nope", 7), 7)
        self.assertIn("price", rows[0])
        self.assertEqual(db.exec("SELECT COUNT(*) FROM t").scalar(), 2)
        self.assertIsNone(db.exec("SELECT id FROM t WHERE id = 99").first())
        self.assertEqual(db.tables(), ["t"])
        info = db.table("t")
        self.assertEqual(info["primary_key"], ["id"])
        self.assertEqual([c["name"] for c in info["columns"]], ["id", "name", "price"])
        self.assertTrue(info["columns"][1]["not_null"])
        with self.assertRaises(KeyError):
            rows[0]["missing"]
        with self.assertRaises(IndexError):
            rows[5]
        db.close()
        self.assertTrue(db.closed)

    def test_composite_primary_key(self):
        db = libsql.Database(self.path)
        db.exec("CREATE TABLE price (sku TEXT, region TEXT, cents INTEGER,"
                " PRIMARY KEY (sku, region), UNIQUE (region, cents))")
        db.exec("CREATE TABLE log (msg TEXT)")
        self.assertEqual(db.table("price")["primary_key"], ["sku", "region"])
        self.assertEqual(db.table("log")["primary_key"], [])
        self.assertEqual([ix["columns"] for ix in db.indexes("price")], [["region", "cents"]])
        db.exec("INSERT INTO price VALUES (?, ?, ?), (?, ?, ?)", ("a", "eu", 100, "a", "us", 120))
        with self.assertRaises(libsql.SqlError):
            db.exec("INSERT INTO price VALUES ('a', 'eu', 1)")
        with self.assertRaises(libsql.SqlError):
            db.exec("INSERT INTO price VALUES ('b', 'eu', 100)")
        self.assertEqual(db.exec("SELECT cents FROM price WHERE sku = ? AND region = ?", ("a", "us")).scalar(), 120)
        self.assertEqual(db.exec("SELECT region FROM price WHERE sku = 'a' ORDER BY region").rows, [("eu",), ("us",)])
        db.close()

    def test_types_round_trip(self):
        db = libsql.Database(self.path)
        db.exec("CREATE TABLE v (i INTEGER, r REAL, t TEXT, b BLOB, d DATETIME, n NULL)")
        when = dt.datetime(2024, 5, 1, 9, 30, 15, 123456, tzinfo=UTC)
        db.exec("INSERT INTO v VALUES (?, ?, ?, ?, ?, ?)",
                (-7, 0.25, "héllo", b"\x00\xff\x00", when, None))
        row = db.exec("SELECT * FROM v")[0]
        self.assertEqual(tuple(row), (-7, 0.25, "héllo", b"\x00\xff\x00", when, None))
        self.assertIsInstance(row["d"], dt.datetime)
        self.assertEqual(row["d"].tzinfo, UTC)
        # bool is an integer; a naive datetime is taken as UTC; a date is midnight.
        db.exec("INSERT INTO v (i, d) VALUES (?, ?)", (True, dt.datetime(2020, 1, 2, 3, 4, 5)))
        db.exec("INSERT INTO v (i, d) VALUES (?, ?)", (False, dt.date(2020, 1, 3)))
        got = db.exec("SELECT i, d FROM v WHERE t IS NULL ORDER BY d").rows
        self.assertEqual(got, [(1, dt.datetime(2020, 1, 2, 3, 4, 5, tzinfo=UTC)),
                               (0, dt.datetime(2020, 1, 3, tzinfo=UTC))])
        # Datetime literals compare as datetimes.
        self.assertEqual(db.exec("SELECT COUNT(*) FROM v WHERE d > '2021-01-01'").scalar(), 1)
        db.close()

    @unittest.skipIf(np is None, "numpy not installed")
    def test_numpy_values_bind(self):
        db = libsql.Database(self.path)
        db.exec("CREATE TABLE v (i INTEGER, r REAL, b BLOB)")
        arr = np.arange(6, dtype=np.float32)
        db.exec("INSERT INTO v VALUES (?, ?, ?)", (np.int64(5), np.float32(1.5), arr))
        row = db.exec("SELECT * FROM v")[0]
        self.assertEqual(row["i"], 5)
        self.assertEqual(row["r"], 1.5)
        self.assertTrue(np.array_equal(np.frombuffer(row["b"], dtype=np.float32), arr))
        with self.assertRaises(TypeError):
            db.exec("INSERT INTO v (b) VALUES (?)", (arr[::2],))  # not contiguous
        db.close()

    def test_errors_carry_codes(self):
        db = libsql.Database(self.path)
        with self.assertRaises(libsql.SqlError) as cm:
            db.exec("SELECT * FROM nowhere")
        self.assertEqual(cm.exception.code, "NoSuchTable")
        self.assertIsInstance(cm.exception, libsql.Error)
        with self.assertRaises(libsql.SqlError) as cm:
            db.exec("SELEC 1")
        self.assertEqual(cm.exception.code, "SyntaxError")
        db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)")
        db.exec("INSERT INTO t (name) VALUES ('x')")
        with self.assertRaises(libsql.SqlError) as cm:
            db.exec("INSERT INTO t (name) VALUES ('x')")
        self.assertEqual(cm.exception.code, "ConstraintViolation")
        with self.assertRaises(TypeError):
            db.exec("INSERT INTO t (name) VALUES (?)", "not-a-sequence")
        db.close()
        with self.assertRaises(ValueError):
            db.exec("SELECT 1")
        # Opening a second writer on the same file is a store-level error.
        a = libsql.Database(self.path)
        with self.assertRaises(libsql.StoreError) as cm:
            libsql.Database(self.path)
        self.assertEqual(cm.exception.code, "Busy")
        a.close()

    def test_transactions(self):
        db = libsql.Database(self.path)
        db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)")
        with db.begin() as t:
            t.exec("INSERT INTO t (v) VALUES (1)")
            t.exec("INSERT INTO t (v) VALUES (2)")
        self.assertEqual(db.exec("SELECT COUNT(*) FROM t").scalar(), 2)
        with self.assertRaises(RuntimeError):
            with db.begin() as t:
                t.exec("INSERT INTO t (v) VALUES (3)")
                raise RuntimeError("boom")
        self.assertEqual(db.exec("SELECT COUNT(*) FROM t").scalar(), 2)  # rolled back
        t = db.begin()
        t.exec("INSERT INTO t (v) VALUES (4)")
        t.rollback()
        self.assertFalse(t.active)
        with self.assertRaises(libsql.SqlError) as cm:
            t.exec("SELECT 1")
        self.assertEqual(cm.exception.code, "BadTransaction")
        r = db.begin_read()
        self.assertTrue(r.read_only)
        self.assertEqual(r.exec("SELECT SUM(v) FROM t").scalar(), 3)
        with self.assertRaises(libsql.SqlError) as cm:
            r.exec("DELETE FROM t")
        self.assertEqual(cm.exception.code, "ReadOnly")
        r.rollback()
        db.close()

    def test_prepared_statements(self):
        db = libsql.Database(self.path)
        db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b TEXT)")
        ins = db.prepare("INSERT INTO t (a, b) VALUES (?, ?)")
        self.assertEqual(ins.parameters, 2)
        self.assertTrue(ins.writes)
        with db.begin() as t:
            for i in range(100):
                ins.exec((i, str(i)), txn=t)
        sel = db.prepare("SELECT b FROM t WHERE a = ?")
        self.assertFalse(sel.writes)
        self.assertEqual(sel.exec((42,)).scalar(), "42")
        self.assertEqual(sel.bind(1, 7).exec().scalar(), "7")
        self.assertIsNone(sel.clear().exec().scalar())  # NULL never equals
        with self.assertRaises(libsql.SqlError):
            ins.bind(3, 1)
        db.close()

    def test_read_only_open(self):
        db = libsql.Database(self.path)
        db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY)")
        db.exec("INSERT INTO t VALUES (1)")
        db.close()
        ro = libsql.Database(self.path, read_only=True)
        self.assertTrue(ro.read_only)
        self.assertEqual(ro.exec("SELECT COUNT(*) FROM t").scalar(), 1)
        with self.assertRaises(libsql.SqlError) as cm:
            ro.exec("INSERT INTO t VALUES (2)")
        self.assertEqual(cm.exception.code, "ReadOnly")
        ro.close()


class Blobs(Scratch):
    def open(self, **kw):
        self.db = libsql.Database(self.path)
        return libsql.BlobStorage(self.db, "blobs", **kw)

    def test_put_get_zero_copy(self):
        blobs = self.open()
        payload = bytes(range(256)) * 40
        blobs.put("k1", payload)
        self.assertEqual(len(blobs), 1)
        self.assertIn("k1", blobs)
        self.assertNotIn("k2", blobs)
        b = blobs["k1"]
        self.assertEqual(len(b), len(payload))
        self.assertEqual(bytes(b), payload)
        self.assertEqual(b.tobytes(), payload)
        self.assertEqual(memoryview(b).tobytes(), payload)
        self.assertTrue(memoryview(b).readonly)
        self.assertEqual(b.name, "k1")
        self.assertEqual(b.offset % 512, 0)
        self.assertTrue(b.has_checksum)
        self.assertTrue(b.verify())
        self.assertIsNone(blobs.get("k2"))
        self.assertEqual(blobs.get("k2", 0), 0)
        with self.assertRaises(KeyError):
            blobs["k2"]
        self.assertTrue(blobs.archive_path.endswith("blobs.tar"))
        self.assertEqual(blobs.indexed_up_to, blobs.archive_size)
        # A blob outlives the storage handle: it pins the mapping it came from.
        del blobs
        self.assertEqual(bytes(b), payload)
        self.db.close()

    @unittest.skipIf(np is None, "numpy not installed")
    def test_numpy_in_and_out_without_copies(self):
        blobs = self.open()
        arr = np.random.default_rng(1).standard_normal((3, 32, 32)).astype(np.float32)
        blobs.put(7, arr)  # int key, C-contiguous buffer straight in
        b = blobs[7]
        self.assertEqual(b.name, "%016x" % 7)
        view = b.numpy("float32", (3, 32, 32))
        self.assertTrue(np.array_equal(view, arr))
        self.assertFalse(view.flags.writeable)
        # Two views land on the same address: nothing was copied out.
        a1 = np.frombuffer(b, dtype=np.uint8).__array_interface__["data"][0]
        a2 = np.frombuffer(b, dtype=np.uint8).__array_interface__["data"][0]
        self.assertEqual(a1, a2)
        with self.assertRaises(TypeError):
            blobs.put(8, arr[:, ::2])  # not contiguous
        self.db.close()

    def test_key_kinds(self):
        blobs = self.open()
        blobs.put(b"\x01\x02", b"bytes-key")
        blobs.put("text", b"str-key")
        blobs.put(2**40, b"int-key")
        self.assertEqual(bytes(blobs[b"\x01\x02"]), b"bytes-key")
        self.assertEqual(blobs[b"\x01\x02"].name, "0102")
        self.assertEqual(bytes(blobs["text"]), b"str-key")
        self.assertEqual(bytes(blobs[2**40]), b"int-key")
        self.assertEqual(bytes(blobs[(2**40).to_bytes(8, "big")]), b"int-key")  # same key bytes
        self.assertEqual(blobs.keys(), sorted([b"\x01\x02", b"text", (2**40).to_bytes(8, "big")]))
        with self.assertRaises(OverflowError):
            blobs.put(-1, b"x")
        with self.assertRaises(TypeError):
            blobs.put(1.5, b"x")
        with self.assertRaises(ValueError):
            blobs.put("x" * 101, b"needs a name")
        blobs.put("x" * 101, b"has one", name="long.bin")
        self.assertEqual(blobs["x" * 101].name, "long.bin")
        self.db.close()

    def test_writer_batches_and_get_many(self):
        blobs = self.open(write_buffer=64 * 1024)
        with blobs.writer() as w:
            for i in range(500):
                w.add(i, (b"%d|" % i) * (1 + i % 300))
        self.assertEqual(len(blobs), 500)
        keys = [499, 3, 250, 1234, 0, 42]
        got = blobs.get_many(keys)
        self.assertEqual(len(got), 6)
        self.assertIsNone(got[3])
        for k, b in zip(keys, got):
            if b is not None:
                self.assertEqual(bytes(b), (b"%d|" % k) * (1 + k % 300))
        self.assertEqual(blobs.prefetch(keys), 5)
        blobs.warm()
        # list() is the archive's own account of itself, in append order.
        listed = blobs.list()
        self.assertEqual(len(listed), 500)
        self.assertEqual(listed[0][0], "%016x" % 0)
        self.assertEqual(listed[10][2], len((b"10|") * 11))
        blobs.finalize()
        self.db.close()

    def test_abandoned_writer_and_catch_up(self):
        blobs = self.open()
        with self.assertRaises(RuntimeError):
            with blobs.writer() as w:
                w.add("lost", b"never indexed")
                raise RuntimeError("stop")
        self.assertEqual(len(blobs), 0)
        self.assertEqual(len(blobs.list()), 1)  # the bytes did land
        self.assertEqual(blobs.catch_up(), 1)
        self.assertEqual(bytes(blobs["lost"]), b"never indexed")  # keyed by member name
        w = blobs.writer()
        w.add("a", b"1")
        w.commit()
        w.commit()  # idempotent
        self.assertFalse(w.active)
        with self.assertRaises(ValueError):
            w.add("b", b"2")
        w2 = blobs.writer()
        w2.add("b", b"2")
        w2.abandon()
        self.assertNotIn("b", blobs)
        self.assertEqual(blobs.catch_up(), 1)
        # A finished Writer or Snapshot pins nothing: closing the database
        # with them still in scope releases the store.
        snap = blobs.snapshot()
        snap.close()
        del blobs
        self.db.close()
        libsql.Database(self.path).close()  # not Busy

    def test_writer_joins_sql_transaction(self):
        blobs = self.open()
        self.db.exec("CREATE TABLE sample (id INTEGER PRIMARY KEY, label INTEGER)")
        with self.db.begin() as t, blobs.writer(t) as w:
            for i in range(10):
                t.exec("INSERT INTO sample VALUES (?, ?)", (i, i % 3))
                w.add(i, b"payload-%d" % i)
        self.assertEqual(self.db.exec("SELECT COUNT(*) FROM sample").scalar(), 10)
        self.assertEqual(len(blobs), 10)
        # Roll the SQL transaction back: rows and index entries both vanish...
        try:
            with self.db.begin() as t, blobs.writer(t) as w:
                t.exec("INSERT INTO sample VALUES (10, 0)")
                w.add(10, b"payload-10")
                raise RuntimeError("abort")
        except RuntimeError:
            pass
        self.assertEqual(self.db.exec("SELECT COUNT(*) FROM sample").scalar(), 10)
        self.assertNotIn(10, blobs)
        # ...while the bytes stay in the archive for catch_up, under the member name.
        self.assertEqual(blobs.catch_up(), 1)
        self.assertEqual(bytes(blobs["%016x" % 10]), b"payload-10")
        # A read transaction is refused.
        r = self.db.begin_read()
        with self.assertRaises(ValueError):
            blobs.writer(r)
        r.rollback()
        # Committing the transaction before the writer is caught.
        t = self.db.begin()
        w = blobs.writer(t)
        t.commit()
        with self.assertRaises(libsql.SqlError):
            w.add(11, b"x")
        self.db.close()

    def test_snapshot_reads_rows_and_blobs_together(self):
        blobs = self.open()
        self.db.exec("CREATE TABLE sample (id INTEGER PRIMARY KEY, label INTEGER)")
        with self.db.begin() as t, blobs.writer(t) as w:
            for i in range(20):
                t.exec("INSERT INTO sample VALUES (?, ?)", (i, i % 4))
                w.add(i, bytes([i]) * 100)
        with self.db.begin_read() as t, blobs.snapshot(t) as snap:
            ids = [r[0] for r in t.exec("SELECT id FROM sample WHERE label = 2 ORDER BY id")]
            self.assertEqual(ids, [2, 6, 10, 14, 18])
            for i, b in zip(ids, snap.get_many(ids)):
                self.assertEqual(bytes(b), bytes([i]) * 100)
            self.assertEqual(snap.prefetch(ids), 5)
            self.assertIn(2, snap)
            self.assertNotIn(99, snap)
            self.assertIsNone(snap.get(99))
            with self.assertRaises(KeyError):
                snap[99]
            self.assertEqual(len(snap.keys()), 20)
            self.assertEqual(snap.keys(prefix=(0).to_bytes(7, "big")), [i.to_bytes(8, "big") for i in range(20)])
        with blobs.snapshot() as snap:
            self.assertEqual(bytes(snap[5]), bytes([5]) * 100)
        self.assertFalse(snap.active)
        self.db.close()

    def test_rebuild_index_undoes_default_naming(self):
        blobs = self.open()
        with blobs.writer() as w:
            for i in range(5):
                w.add(i, b"i%d" % i)
            w.add(b"\xaa\xbb", b"bytes")
        self.assertEqual(blobs.rebuild_index(key_of="int"), 6)
        for i in range(5):
            self.assertEqual(bytes(blobs[i]), b"i%d" % i)
        self.assertFalse(blobs[0].has_checksum)  # header-only rebuild
        self.assertTrue(blobs[0].verify())
        self.assertEqual(blobs.rebuild_index(key_of="hex"), 6)
        self.assertEqual(bytes(blobs[b"\xaa\xbb"]), b"bytes")
        self.assertEqual(blobs.rebuild_index(key_of=lambda name: "n:" + name), 6)
        self.assertEqual(bytes(blobs["n:" + "%016x" % 3]), b"i3")
        self.assertEqual(blobs.rebuild_index(), 6)
        self.assertEqual(bytes(blobs["%016x" % 3]), b"i3")
        self.assertTrue(blobs.erase("%016x" % 3))
        self.assertFalse(blobs.erase("%016x" % 3))
        self.assertEqual(len(blobs), 5)
        self.db.close()

    def test_close_waits_for_blob_handles(self):
        blobs = self.open()
        blobs.put("k", b"v")
        self.db.close()  # deferred: the BlobStorage still borrows the store
        self.assertTrue(self.db.closed)
        with self.assertRaises(ValueError):
            self.db.exec("SELECT 1")
        with self.assertRaises(ValueError):
            libsql.BlobStorage(self.db, "other")
        self.assertEqual(bytes(blobs["k"]), b"v")  # still fully usable
        with self.assertRaises(libsql.StoreError):
            libsql.Database(self.path)  # Busy: the file is still held
        del blobs
        db = libsql.Database(self.path)  # released with the last handle
        for bad in ("sql_catalog", "tbl:x", "idx:y", ""):
            with self.assertRaises(ValueError):
                libsql.BlobStorage(db, bad)
        with self.assertRaises(ValueError):
            libsql.BlobStorage(db, "b", access="fast")
        db.close()

    def test_options(self):
        side = os.path.join(self.dir, "bulk")
        blobs = self.open(directory=side, file_name="pixels.tar", access="random",
                          sync_on_append=True, write_buffer=0)
        blobs.put("k", b"v")
        self.assertEqual(blobs.archive_path, os.path.join(side, "pixels.tar"))
        self.assertEqual(bytes(blobs["k"]), b"v")
        del blobs
        self.db.close()
        ro = libsql.Database(self.path, read_only=True)
        blobs = libsql.BlobStorage(ro, "blobs", directory=side, file_name="pixels.tar")
        self.assertTrue(blobs.read_only)
        self.assertEqual(bytes(blobs["k"]), b"v")
        with self.assertRaises(libsql.StoreError) as cm:
            blobs.put("k2", b"v2")
        self.assertEqual(cm.exception.code, "ReadOnly")
        del blobs
        ro.close()


def _worker(path, n, out):
    """A DataLoader-style worker: its own read-only handles, opened after the fork."""
    try:
        db = libsql.Database(path, read_only=True)
        blobs = libsql.BlobStorage(db, "blobs", access="random")
        ok = 0
        with blobs.snapshot() as snap:
            for i in range(n):
                if bytes(snap[i]) == bytes([i % 251]) * (100 + i):
                    ok += 1
        out.put(ok)
    except Exception as e:  # pragma: no cover - reported through the queue
        out.put(repr(e))


class MultiProcess(Scratch):
    def test_read_only_workers_share_the_store(self):
        db = libsql.Database(self.path)
        blobs = libsql.BlobStorage(db, "blobs")
        n = 200
        with blobs.writer() as w:
            for i in range(n):
                w.add(i, bytes([i % 251]) * (100 + i))
        del blobs
        db.close()

        # The parent keeps a read-only handle open too, as a training script
        # that read len(dataset) before spawning workers would.
        parent = libsql.Database(self.path, read_only=True)
        ctx = multiprocessing.get_context("fork")
        out = ctx.Queue()
        procs = [ctx.Process(target=_worker, args=(self.path, n, out)) for _ in range(3)]
        for p in procs:
            p.start()
        results = [out.get(timeout=60) for _ in procs]
        for p in procs:
            p.join(timeout=60)
        self.assertEqual(results, [n, n, n])
        parent.close()


if __name__ == "__main__":
    unittest.main()
