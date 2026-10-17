-- Index-only (covering) reads must return exactly what the rows hold.
CREATE TABLE t (a, b, c TEXT COLLATE NOCASE, d);
CREATE INDEX ta ON t (a);
CREATE INDEX tbd ON t (b DESC, d);
CREATE INDEX tc ON t (c);
INSERT INTO t VALUES (1, 'x', 'Abc', 1.0), (1.0, 'y', 'abc', 2), (2.5, NULL, 'ABC ', -0.0),
  ('1', x'00ff', 'b', 9223372036854775807), (NULL, 'x', NULL, -9223372036854775808),
  (9007199254740993, 'z', 'c', 3.0), (-1.0, 'z', 'C', 1e300), (0.0, '', 'd', 4);
SELECT a, typeof(a) FROM t WHERE a = 1 ORDER BY rowid;
SELECT a, typeof(a) FROM t WHERE a > 0 ORDER BY rowid;
SELECT a, typeof(a), rowid FROM t ORDER BY 3;
SELECT DISTINCT a, typeof(a) FROM t ORDER BY 1, 2;
SELECT count(*) FROM t WHERE a = 1;
SELECT b, d, typeof(d) FROM t WHERE b = 'z' ORDER BY d;
SELECT b, d, typeof(d) FROM t WHERE b > 'a' ORDER BY b, d;
SELECT d, typeof(d) FROM t WHERE b IS NULL;
SELECT c FROM t WHERE c = 'abc' ORDER BY rowid;
SELECT c, typeof(c) FROM t ORDER BY rowid;
SELECT count(*), sum(a) FROM t WHERE a BETWEEN 0 AND 3;
UPDATE t SET a = 1.0 WHERE rowid = 1;
SELECT a, typeof(a) FROM t WHERE a = 1 ORDER BY rowid;
UPDATE t SET a = 1 WHERE rowid = 2;
SELECT a, typeof(a) FROM t WHERE a = 1 ORDER BY rowid;
UPDATE t SET d = 2.0 WHERE rowid = 2;
SELECT b, d, typeof(d) FROM t WHERE b = 'y';
PRAGMA integrity_check;

-- Clustered and WITHOUT ROWID tables read their key from the entry suffix.
CREATE TABLE k (x REAL, y TEXT, z, PRIMARY KEY (x, y));
CREATE INDEX kz ON k (z);
INSERT INTO k VALUES (1.0, 'p', 10), (2.5, 'q', 20), (3, 'r', 10), (-0.5, 's', 30);
SELECT z, x, typeof(x), y FROM k WHERE z = 10 ORDER BY x;
SELECT x, y FROM k WHERE z >= 20 ORDER BY z;
SELECT z, x FROM k ORDER BY 1, 2;
CREATE TABLE w (p, q, r, PRIMARY KEY (p DESC)) WITHOUT ROWID;
CREATE INDEX wq ON w (q);
INSERT INTO w VALUES (1.0, 'a', 1), (2, 'b', 2), ('t', 'a', 3), (x'01', 'c', 4);
SELECT q, p, typeof(p) FROM w WHERE q = 'a' ORDER BY p;
SELECT q, p FROM w ORDER BY q, p;
SELECT r FROM w WHERE q = 'b';
UPDATE w SET p = 7.0 WHERE p = 2;
SELECT q, p, typeof(p) FROM w WHERE q = 'b';
PRAGMA integrity_check;

-- Joins and aggregates over covered sources.
CREATE TABLE cat (id INTEGER PRIMARY KEY, name);
CREATE TABLE itm (id INTEGER PRIMARY KEY, cat_id, label, price);
CREATE INDEX itm_cat ON itm (cat_id, price);
INSERT INTO cat VALUES (1, 'one'), (2, 'two');
INSERT INTO itm VALUES (1, 1, 'a', 10), (2, 1, 'b', 20.0), (3, 2, 'c', 30), (4, 2, 'd', NULL);
SELECT c.name, i.price FROM cat c JOIN itm i ON i.cat_id = c.id ORDER BY 1, 2;
SELECT cat_id, count(*), sum(price), max(price) FROM itm GROUP BY cat_id;
SELECT c.name, (SELECT max(price) FROM itm WHERE cat_id = c.id) FROM cat c ORDER BY 1;
SELECT cat_id, price FROM itm WHERE cat_id = 1 AND price > 15;
SELECT id, cat_id FROM itm WHERE cat_id IN (2, 1) ORDER BY id;
SELECT count(*) FROM itm;
SELECT count(*) FROM itm WHERE cat_id = 2;
DELETE FROM itm WHERE price IS NULL;
SELECT count(*) FROM itm;
SELECT i.label FROM itm i WHERE i.cat_id = 2;

-- DISTINCT over an index's leading columns skips each distinct prefix.
CREATE TABLE dd (a, b, c, d TEXT COLLATE NOCASE);
CREATE INDEX dd_ab ON dd (a, b);
CREATE INDEX dd_c ON dd (c COLLATE NOCASE);
CREATE INDEX dd_d ON dd (d);
INSERT INTO dd VALUES (1, 1, 'x', 'Q'), (1, 2, 'X', 'q'), (2, 1, 'y', 'r'), (1, 1, 'z', 'R'),
  (NULL, 3, 'Y', NULL), (2, 2, NULL, 'q'), (1.0, 2, 'x', 's'), ('1', 1, 'x', 'S'), (x'01', 9, 'w', 't');
SELECT DISTINCT a FROM dd ORDER BY 1;
SELECT DISTINCT a, typeof(a) FROM dd ORDER BY 1, 2;
SELECT DISTINCT a, b FROM dd ORDER BY 1, 2;
SELECT DISTINCT b, a FROM dd ORDER BY 1, 2;
SELECT DISTINCT b FROM dd ORDER BY 1;
SELECT DISTINCT c FROM dd ORDER BY 1;
SELECT DISTINCT d FROM dd ORDER BY 1;
SELECT DISTINCT a FROM dd WHERE b > 1 ORDER BY 1;
SELECT count(*) FROM (SELECT DISTINCT a FROM dd);
SELECT DISTINCT a, a FROM dd ORDER BY 1;

-- Exact range windows: strict and inclusive bounds, NULLs below a high-only
-- bound, BETWEEN mixed with other bounds, DESC and NOCASE parts, huge rowids.
CREATE TABLE rg (id INTEGER PRIMARY KEY, v, w TEXT COLLATE NOCASE, x);
CREATE INDEX rg_v ON rg (v);
CREATE INDEX rg_w ON rg (w);
CREATE INDEX rg_x ON rg (x DESC);
INSERT INTO rg VALUES (1, 5, 'a', 1), (2, 5.0, 'B', 2), (3, 6, 'c', NULL), (4, NULL, NULL, 3),
  (5, '5', 'A', 4), (6, 4.5, '_', 5), (7, x'05', 'b', 6), (9223372036854775807, 7, 'z', 7),
  (9223372036854775000, 8, 'y', 8), (-9223372036854775808, 3, 'x', 9);
SELECT id FROM rg WHERE v > 5 ORDER BY id;
SELECT id FROM rg WHERE v >= 5 ORDER BY id;
SELECT id FROM rg WHERE v < 5 ORDER BY id;
SELECT id FROM rg WHERE v <= 5 ORDER BY id;
SELECT id FROM rg WHERE v < 6 AND v > 4 ORDER BY id;
SELECT id FROM rg WHERE v BETWEEN 4 AND 6 AND v >= 5 ORDER BY id;
SELECT id FROM rg WHERE v BETWEEN 4 AND 6 AND v <= 5 ORDER BY id;
SELECT id FROM rg WHERE v > '4' ORDER BY id;
SELECT id FROM rg WHERE v < x'00' ORDER BY id;
SELECT id FROM rg WHERE w > 'a' ORDER BY id;
SELECT id FROM rg WHERE w < 'b' ORDER BY id;
SELECT id FROM rg WHERE x > 3 ORDER BY id;
SELECT id FROM rg WHERE x <= 3 ORDER BY id;
SELECT id FROM rg WHERE id > 9223372036854775000 ORDER BY id;
SELECT id FROM rg WHERE id >= 9223372036854775000 AND id < 9223372036854775807 ORDER BY id;
SELECT id FROM rg WHERE id > 5.5 ORDER BY id;
SELECT id FROM rg WHERE id < 2.0 ORDER BY id;
SELECT id FROM rg WHERE id <= -9223372036854775808 ORDER BY id;
SELECT id FROM rg WHERE id > 9.3e18 ORDER BY id;
SELECT id FROM rg WHERE id < -9.3e18 ORDER BY id;
SELECT id FROM rg WHERE id > 'abc' ORDER BY id;
SELECT id FROM rg WHERE id BETWEEN 2 AND 5 AND id > 3 ORDER BY id;
SELECT count(*) FROM rg WHERE v > 4 AND v < 8;
