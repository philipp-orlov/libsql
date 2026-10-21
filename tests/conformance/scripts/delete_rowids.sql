-- DELETE that decodes only what finds and removes each row, and rowid
-- high-water marks written once per transaction: through savepoints, failed
-- statements, triggers and foreign keys, every answer must be SQLite's.
CREATE TABLE d (k INTEGER, s INTEGER, b BLOB, t TEXT, PRIMARY KEY (k, s));
CREATE INDEX d_t ON d (t);
WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM n WHERE i < 299)
INSERT INTO d SELECT i / 30, i % 30, zeroblob(200 + i), 't' || (i % 7) FROM n;
DELETE FROM d WHERE k = 2;
SELECT count(*), sum(length(b)) FROM d;
DELETE FROM d WHERE k = 3 AND length(b) > 300;
SELECT k, s FROM d WHERE k = 3 ORDER BY s;
DELETE FROM d WHERE t = 't3' AND k < 5;
SELECT k, s, t FROM d WHERE k < 5 AND t IN ('t2', 't3', 't4') ORDER BY k, s;
DELETE FROM d WHERE k = 6 AND s > 20 RETURNING k, s, length(b), t;
DELETE FROM d WHERE k = 7 AND s IN (SELECT s FROM d WHERE k = 8 AND s < 4);
SELECT k, s FROM d WHERE k = 7 ORDER BY s LIMIT 6;
SELECT count(*) FROM d;
PRAGMA integrity_check;

-- With a trigger, rows reload as before.
CREATE TABLE log (what TEXT);
CREATE TABLE g (id INTEGER PRIMARY KEY, p INTEGER, v BLOB);
CREATE TRIGGER g_del AFTER DELETE ON g BEGIN
  INSERT INTO log VALUES ('gone ' || old.id || ' ' || length(old.v));
  UPDATE g SET v = x'ff' WHERE p = old.id;
END;
INSERT INTO g VALUES (1, NULL, x'01'), (2, 1, x'0202'), (3, 2, x'030303'), (4, NULL, x'04');
DELETE FROM g WHERE id <= 2;
SELECT * FROM log ORDER BY what;
SELECT id, hex(v) FROM g ORDER BY id;

-- With foreign keys on, rows reload whole for the cascade.
CREATE TABLE par (id INTEGER PRIMARY KEY, name TEXT, v BLOB);
CREATE TABLE chi (id INTEGER PRIMARY KEY, par_id INTEGER REFERENCES par (id) ON DELETE CASCADE, name TEXT);
INSERT INTO par VALUES (1, 'a', x'aa'), (2, 'b', x'bb'), (3, 'c', x'cc');
INSERT INTO chi VALUES (10, 1, 'x'), (11, 2, 'y'), (12, 2, 'z');
PRAGMA foreign_keys = ON;
DELETE FROM par WHERE id >= 2;
SELECT * FROM chi ORDER BY id;
PRAGMA foreign_keys = OFF;

-- AUTOINCREMENT marks across statements, savepoints and failures.
CREATE TABLE a (id INTEGER PRIMARY KEY AUTOINCREMENT, u TEXT UNIQUE);
BEGIN;
INSERT INTO a (u) VALUES ('p');
INSERT INTO a (u) VALUES ('q');
SAVEPOINT s1;
INSERT INTO a (u) VALUES ('r');
ROLLBACK TO s1;
INSERT INTO a (u) VALUES ('s');
COMMIT;
SELECT * FROM sqlite_sequence;
SELECT id, u FROM a ORDER BY id;
BEGIN;
INSERT INTO a (u) VALUES ('t');
INSERT INTO a (u) SELECT 'v' UNION ALL SELECT 'p';
INSERT INTO a (u) VALUES ('w');
COMMIT;
SELECT * FROM sqlite_sequence;
SELECT id, u FROM a ORDER BY id;
DELETE FROM a WHERE u = 'w';
INSERT INTO a (u) VALUES ('x');
SELECT * FROM sqlite_sequence;
BEGIN;
INSERT INTO a (u) VALUES ('y');
CREATE TABLE a2 (id INTEGER PRIMARY KEY AUTOINCREMENT, z TEXT);
INSERT INTO a2 (z) VALUES ('one');
DROP TABLE a2;
INSERT INTO a (u) VALUES ('zz');
COMMIT;
SELECT * FROM sqlite_sequence;
SELECT id, u FROM a ORDER BY id;

-- A table with a declared key and a hidden rowid.
CREATE TABLE h (name TEXT PRIMARY KEY, n INTEGER);
BEGIN;
INSERT INTO h VALUES ('a', 1);
INSERT INTO h VALUES ('b', 2);
SAVEPOINT s2;
INSERT INTO h VALUES ('c', 3);
RELEASE s2;
COMMIT;
INSERT INTO h VALUES ('d', 4);
SELECT rowid, name, n FROM h ORDER BY rowid;
