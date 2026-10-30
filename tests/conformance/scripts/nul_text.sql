-- Text holding NUL bytes: which functions see the whole value and which stop
-- at the first NUL (SQLite's C-string readers).
CREATE TABLE n (t TEXT, b BLOB);
INSERT INTO n VALUES (CAST(x'61006263' AS TEXT), x'610062632020');
SELECT hex(trim(t)), hex(ltrim(t, 'a')), hex(rtrim(t, 'c')), hex(trim(b)) FROM n;
SELECT instr(t, 'b'), instr(t, x'00'), instr(b, 'c'), instr(t, CAST(x'0062' AS TEXT)) FROM n;
SELECT quote(t), hex(quote(t)), length(t), octet_length(t), hex(t) FROM n;
SELECT hex(upper(t)), hex(lower(t)), hex(replace(t, 'b', 'X')), hex(substr(t, 1, 3)) FROM n;
SELECT t LIKE 'a%', t GLOB 'a*', t LIKE '%c', hex(printf('%s', t)), hex(printf('%.2s', t)) FROM n;
SELECT unicode(t), soundex(t), hex(t || 'z'), hex(concat(t, 'z')), hex(concat_ws(',', t, t)) FROM n;
SELECT hex(group_concat(t)), hex(group_concat(t, x'00')), count(DISTINCT t) FROM n;
SELECT hex(json_quote(t)), json_valid(t), hex(CAST(t AS BLOB)), CAST(t AS INTEGER), CAST(t AS REAL) FROM n;
SELECT t = CAST(x'61' AS TEXT), t > 'a', t < 'ab', min(t), hex(max(t)) FROM n;
SELECT hex(format('%q', t)), hex(format('%Q', t)), hex(format('%w', t)), hex(char(97, 0, 98)) FROM n;
SELECT hex(substr(t, 2)), hex(substr(t, -2)), hex(substring(b, 2, 2)), hex(ltrim(b, 'a')) FROM n;
SELECT hex(upper(b)), hex(trim(b, ' ')), instr(b, x'0062'), length(b), hex(replace(b, x'00', 'Z')) FROM n;
SELECT typeof(replace(x'00ff', '', 'z')), typeof(replace(x'00ff', x'00', 'z')), typeof(replace(5, x'00', 'z')),
       typeof(replace(5, '', 'z')), typeof(replace(1.5, '', 'z')), replace(1.5, '', 'z');
SELECT like('a%', t), glob('a*', t), like('%c', t), like('%c', b), glob('*c*', b) FROM n;
SELECT hex(printf('%c', t)), hex(printf('%5s|', t)), printf('%d', t) FROM n;
SELECT instr(x'bb' || 'abc', 'a'), instr(x'00ff' || 'abc', 'a'), instr(CAST(x'c3a9' AS TEXT), CAST(x'a9' AS TEXT)),
       instr('héllo', 'l'), instr('', ''), instr('abc', ''), instr(x'0102', x'02'), instr(x'bbbb61', 'a');
