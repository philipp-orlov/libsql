-- Equality joins with no usable index: the inner side is cached once and
-- bucketed by its join key. Affinity, collation and NULLs must decide matches
-- exactly as the comparison would.
CREATE TABLE l (id INTEGER PRIMARY KEY, i INTEGER, t TEXT, n NUMERIC, b BLOB, r REAL, any);
CREATE TABLE q (k INTEGER, s TEXT COLLATE NOCASE, x, y TEXT COLLATE RTRIM, v);
WITH RECURSIVE g(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM g WHERE i < 400)
INSERT INTO l SELECT i, i % 37, 'v' || (i % 23), CASE WHEN i % 3 = 0 THEN i % 11 ELSE (i % 11) || '' END,
       CAST('b' || (i % 5) AS BLOB), (i % 13) * 1.0, CASE i % 4 WHEN 0 THEN i % 9 WHEN 1 THEN (i % 9) || '' WHEN 2 THEN (i % 9) * 1.0 ELSE NULL END FROM g;
WITH RECURSIVE g(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM g WHERE i < 300)
INSERT INTO q SELECT CASE WHEN i % 10 = 0 THEN NULL ELSE i % 41 END, CASE i % 3 WHEN 0 THEN 'V' || (i % 23) ELSE 'v' || (i % 23) END,
       CASE i % 5 WHEN 0 THEN i % 9 WHEN 1 THEN (i % 9) || '' WHEN 2 THEN (i % 9) * 1.0 WHEN 3 THEN NULL ELSE x'00' END,
       CASE i % 2 WHEN 0 THEN 'v' || (i % 23) || '  ' ELSE 'v' || (i % 23) END, i FROM g;
SELECT count(*), sum(l.id), sum(q.v) FROM l JOIN q ON q.k = l.i;
SELECT count(*), sum(l.id * q.v) FROM l JOIN q ON l.i = q.k WHERE q.v > 100;
SELECT count(*) FROM l JOIN q ON q.s = l.t;
SELECT count(*) FROM l JOIN q ON l.t = q.s;
SELECT count(*) FROM l JOIN q ON q.y = l.t;
SELECT count(*) FROM l JOIN q ON l.t = q.y;
SELECT count(*) FROM l JOIN q ON q.x = l.i;
SELECT count(*) FROM l JOIN q ON q.x = l.t;
SELECT count(*) FROM l JOIN q ON q.x = l.n;
SELECT count(*) FROM l JOIN q ON q.x = l.r;
SELECT count(*) FROM l JOIN q ON q.x = l.any;
SELECT count(*) FROM l JOIN q ON l.any = q.x;
SELECT count(*) FROM l JOIN q ON q.k = l.n;
SELECT count(*) FROM l JOIN q ON q.k = l.r;
SELECT count(*) FROM l JOIN q ON q.k = l.t;
SELECT count(*) FROM l JOIN q ON q.v = l.b;
SELECT count(*) FROM l JOIN q ON q.k = l.i AND q.s = l.t;
SELECT count(*) FROM l JOIN q ON q.k = l.i + 1 AND q.v % 2 = 0;
SELECT count(*) FROM l JOIN q ON q.k = l.i COLLATE NOCASE;
SELECT count(*) FROM l JOIN q ON q.s = l.t COLLATE BINARY;
SELECT count(*) FROM l, q WHERE q.k = l.i AND l.id < 50;
SELECT l.id, q.v FROM l JOIN q ON q.k = l.i WHERE l.id BETWEEN 30 AND 34 ORDER BY 1, 2;
SELECT count(*) FROM l JOIN q ON q.k = l.i JOIN l AS m ON m.i = q.k + 1;
SELECT l.id, (SELECT count(*) FROM q JOIN l AS z ON z.i = q.k WHERE q.v < l.id) FROM l WHERE l.id < 6 ORDER BY 1;
SELECT count(*) FROM l LEFT JOIN q ON q.k = l.i;
SELECT count(*) FROM l JOIN q ON q.k IS l.i;
