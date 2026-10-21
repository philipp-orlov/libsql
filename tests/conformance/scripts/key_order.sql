-- ORDER BY that the access path already delivers (no sort), backward walks
-- for ORDER BY ... DESC over a unique key, and min()/max() found by a seek:
-- every answer must be the one a full scan and sort gives.
CREATE TABLE e (record_id INTEGER NOT NULL, slot INTEGER NOT NULL, v BLOB NOT NULL, PRIMARY KEY (record_id, slot));
WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM n WHERE i < 599)
INSERT INTO e SELECT i / 50, (i * 7) % 50, CAST('v' || i AS BLOB) FROM n;
SELECT slot, hex(v) FROM e WHERE record_id = 3 ORDER BY slot;
SELECT slot FROM e WHERE record_id = 3 ORDER BY slot DESC;
SELECT slot FROM e WHERE record_id = 3 ORDER BY slot DESC LIMIT 1;
SELECT slot FROM e WHERE record_id = 3 ORDER BY record_id, slot LIMIT 5 OFFSET 3;
SELECT slot FROM e WHERE record_id = 3 ORDER BY slot, record_id DESC LIMIT 4;
SELECT slot FROM e WHERE record_id = 3 AND slot > 40 ORDER BY slot;
SELECT slot FROM e WHERE record_id = 3 AND slot BETWEEN 10 AND 20 ORDER BY slot DESC;
SELECT slot FROM e WHERE record_id = 3 ORDER BY slot DESC LIMIT 3 OFFSET 2;
SELECT slot FROM e WHERE record_id = 99 ORDER BY slot;
SELECT record_id, slot FROM e WHERE record_id IN (2, 4) ORDER BY slot LIMIT 6;
SELECT record_id, slot FROM e WHERE slot = 7 ORDER BY record_id;
SELECT record_id, slot FROM e WHERE record_id > 9 ORDER BY record_id, slot DESC LIMIT 7;
SELECT record_id, slot FROM e WHERE record_id >= 10 ORDER BY record_id LIMIT 3;
SELECT max(slot) FROM e WHERE record_id = 3;
SELECT min(slot) FROM e WHERE record_id = 3;
SELECT max(slot) + 1, min(slot) FROM e WHERE record_id = 3;
SELECT max(slot) FROM e WHERE record_id = 99;
SELECT max(slot) FROM e WHERE record_id = 3 AND slot < 30;
SELECT min(slot) FROM e WHERE record_id = 3 AND slot > 30;
SELECT max(slot), hex(v) FROM e WHERE record_id = 3;
SELECT max(slot) FROM e WHERE record_id = 3 HAVING max(slot) > 10;
SELECT max(record_id) FROM e WHERE record_id < 5;
SELECT min(record_id) FROM e WHERE record_id > 5;
SELECT count(*) FROM e WHERE record_id = 3 ORDER BY 1;

-- A rowid table: the rowid and its alias.
CREATE TABLE r (id INTEGER PRIMARY KEY, k INTEGER, s TEXT COLLATE NOCASE);
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 300)
INSERT INTO r SELECT i * 3, CASE WHEN i % 7 = 0 THEN NULL ELSE i % 13 END,
       CASE i % 3 WHEN 0 THEN 'a' WHEN 1 THEN 'A' ELSE 'b' END FROM n;
SELECT id FROM r WHERE id > 800 ORDER BY id;
SELECT id FROM r WHERE id > 800 ORDER BY id DESC;
SELECT id FROM r WHERE id BETWEEN 100 AND 130 ORDER BY rowid DESC LIMIT 2;
SELECT id FROM r WHERE id < 20 ORDER BY id DESC LIMIT 1;
SELECT id FROM r WHERE id > 1.5 AND id < 10.5 ORDER BY id;
SELECT id FROM r WHERE id > 'x' ORDER BY id;
SELECT max(id) FROM r WHERE id < 500;
SELECT min(id) FROM r WHERE id > 500;
SELECT max(rowid) FROM r WHERE rowid < 100.5;
SELECT min(id) FROM r WHERE id > 100000;

-- Indexes: a covering read, a row read, NULLs, NOCASE and descending parts.
CREATE INDEX r_k ON r (k);
CREATE INDEX r_s ON r (s);
CREATE INDEX r_ks ON r (k DESC, s);
SELECT k, id FROM r WHERE k = 4 ORDER BY id;
SELECT k FROM r WHERE k > 10 ORDER BY k;
SELECT k FROM r WHERE k < 3 ORDER BY k;
SELECT k FROM r WHERE k < 3 ORDER BY k NULLS LAST;
SELECT k FROM r WHERE k <= 2 ORDER BY k DESC;
SELECT s, id FROM r WHERE s = 'a' ORDER BY s, id LIMIT 6;
SELECT s FROM r WHERE s > 'a' ORDER BY s LIMIT 5;
SELECT s FROM r WHERE s > 'a' ORDER BY s COLLATE BINARY LIMIT 5;
SELECT min(k) FROM r WHERE k < 5;
SELECT max(k) FROM r WHERE k < 5;
SELECT min(k) FROM r INDEXED BY r_k WHERE k IS NOT NULL;
SELECT min(s), max(s) FROM r WHERE s > '';
SELECT max(k) FROM r WHERE k > 100;
SELECT min(k) FROM r WHERE k IN (3, 5);

-- Mixed classes under one key part, NULLs among them.
CREATE TABLE m (a INTEGER, b, c TEXT, PRIMARY KEY (a, b, c)) WITHOUT ROWID;
INSERT INTO m VALUES (1, 1, 'x'), (1, 2.5, 'y'), (1, 'two', 'z'), (1, x'00', 'w'), (1, -3, 'v');
INSERT INTO m VALUES (2, 1.0, 'a'), (2, 1, 'b'), (2, 7, 'c');
INSERT INTO m VALUES (3, 5, 'a'), (3, 5.0, 'b'), (3, 2, 'c');
SELECT b, c FROM m WHERE a = 1 ORDER BY b;
SELECT b, c FROM m WHERE a = 1 ORDER BY b, c DESC;
SELECT b, c FROM m WHERE a = 1 ORDER BY b DESC, c DESC;
SELECT b, c FROM m WHERE a = 2 ORDER BY b, c;
SELECT min(b), typeof(min(b)) FROM m WHERE a = 1;
SELECT max(b), typeof(max(b)) FROM m WHERE a = 1;
SELECT min(b), typeof(min(b)) FROM m WHERE a = 2;
SELECT max(b) FROM m WHERE a = 3;
SELECT max(c) FROM m WHERE a = 2 AND b = 1;
CREATE TABLE z (a INTEGER, b);
CREATE INDEX z_ab ON z (a, b);
INSERT INTO z VALUES (1, NULL), (1, NULL), (2, NULL), (2, 4), (2, 4.0), (2, NULL);
SELECT max(b) FROM z WHERE a = 1;
SELECT min(b) FROM z WHERE a = 1;
SELECT min(b) FROM z WHERE a = 2;
SELECT max(b) FROM z WHERE a = 2;
SELECT b FROM z WHERE a = 2 ORDER BY b;
SELECT b FROM z WHERE a = 2 ORDER BY b NULLS LAST;

-- Writes through the same shapes.
DELETE FROM e WHERE record_id = 4 AND slot > (SELECT max(slot) - 5 FROM e WHERE record_id = 4);
SELECT slot FROM e WHERE record_id = 4 ORDER BY slot DESC LIMIT 3;
UPDATE r SET k = 100 WHERE id IN (SELECT id FROM r WHERE id > 10 ORDER BY id LIMIT 2);
SELECT id, k FROM r WHERE id > 0 ORDER BY id LIMIT 4;
DELETE FROM r WHERE id > 850;
SELECT max(id) FROM r WHERE id > 0;

-- Index entries end with the row key: negative rowids, a clustered key.
CREATE TABLE neg (id INTEGER PRIMARY KEY, k INTEGER);
CREATE INDEX neg_k ON neg (k);
INSERT INTO neg VALUES (-9223372036854775808, 1), (-300, 1), (-2, 2), (0, 1), (7, 2), (9223372036854775807, 1);
SELECT id FROM neg WHERE k = 1 ORDER BY id;
SELECT id FROM neg WHERE k = 1 ORDER BY id LIMIT 2 OFFSET 1;
SELECT id FROM neg WHERE id > -1000 ORDER BY id DESC;
SELECT min(id) FROM neg WHERE id < 100;
SELECT max(id) FROM neg WHERE id < 0;
SELECT min(id) FROM neg WHERE k = 1;
SELECT max(id) FROM neg WHERE k = 2;
CREATE INDEX m_c ON m (c);
SELECT a, b, c FROM m WHERE c = 'a' ORDER BY a, b;
SELECT a, b FROM m WHERE c = 'b' ORDER BY a;
