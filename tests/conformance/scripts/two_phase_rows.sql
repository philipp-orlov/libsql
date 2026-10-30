-- Rows read in two phases: the columns the filters (or an ORDER BY ... LIMIT's
-- keys) need first, the rest only for rows that pass. Every shape that reads
-- a row some other way must still see it whole.
CREATE TABLE w (id INTEGER PRIMARY KEY, a INTEGER, b TEXT, c REAL, d BLOB, e);
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 2000)
INSERT INTO w SELECT i, i % 17, 'b-' || (i * 7919 % 2000), (i % 13) * 0.5, x'00ff', CASE WHEN i % 5 = 0 THEN NULL ELSE i END FROM n;
ALTER TABLE w ADD COLUMN f TEXT DEFAULT 'dflt';
ALTER TABLE w ADD COLUMN g INTEGER DEFAULT 42;
UPDATE w SET f = 'set' WHERE id % 3 = 0;
SELECT id, b, c, d, e, f, g FROM w WHERE a = 3 ORDER BY id LIMIT 20;
SELECT id, f, g FROM w WHERE c > 5.5 AND a < 4 ORDER BY id;
SELECT * FROM w WHERE e IS NULL AND a = 0 ORDER BY id;
SELECT id, b, f FROM w ORDER BY c DESC, b LIMIT 15;
SELECT id, b, f, g FROM w ORDER BY c, e DESC LIMIT 25 OFFSET 7;
SELECT id, b FROM w ORDER BY b LIMIT 10;
SELECT b, f FROM w WHERE id > 1990 ORDER BY g, id LIMIT 3;
SELECT id, f FROM w WHERE a = 1 AND EXISTS (SELECT 1 FROM w AS v WHERE v.id = w.id + 1 AND v.f = 'set') ORDER BY id;
SELECT id, (SELECT count(*) FROM w AS v WHERE v.a = w.a AND v.id < w.id) AS before FROM w WHERE c = 0 ORDER BY id LIMIT 12;
SELECT w.id, w.b, x.b FROM w LEFT JOIN w AS x ON x.id = w.id + 1000 AND x.a = 2 WHERE w.a = 2 ORDER BY w.id;
SELECT w.id, x.f FROM w JOIN w AS x ON x.a = w.a AND x.id = w.id + 17 WHERE w.c > 6 ORDER BY w.id LIMIT 20;
SELECT a, count(*), min(b), max(f), sum(g) FROM w WHERE c < 1 GROUP BY a ORDER BY a;
SELECT DISTINCT f, g FROM w WHERE a < 3 ORDER BY f;
SELECT id, b FROM w WHERE a = 5 UNION ALL SELECT id, f FROM w WHERE a = 6 ORDER BY 1 LIMIT 10;
SELECT id, b, row_number() OVER (ORDER BY c, id) FROM w WHERE a = 7 ORDER BY id LIMIT 8;
UPDATE w SET b = b || '!' WHERE c > 6;
DELETE FROM w WHERE a = 16 AND f = 'dflt';
SELECT count(*), sum(length(b)), count(f), sum(g) FROM w;
SELECT id, b, f FROM w WHERE c > 6 ORDER BY b DESC LIMIT 5;
CREATE TABLE k (p TEXT PRIMARY KEY, q INTEGER, r TEXT) WITHOUT ROWID;
INSERT INTO k SELECT 'k' || id, a, b FROM w WHERE id < 500;
SELECT p, q, r FROM k WHERE q = 4 ORDER BY r LIMIT 6;
SELECT p, r FROM k ORDER BY q DESC, p LIMIT 9;
