// Copyright (c) 2026 Philipp Orlov

// Dataset SQL names its parameters (`WHERE year = :year`); the wire protocol of the SQL route takes positional values.
// This rewrites :name, @name and $name (outside strings, quoted identifiers and comments) to ?1, ?2, ... and returns
// the names in that order. Free of Angular imports on purpose.

export interface BoundSql {
  sql: string;
  /** names[i] is the parameter ?(i+1). */
  names: string[];
}

export function bindNames(sql: string): BoundSql {
  const names: string[] = [];
  let out = '';
  let i = 0;
  const n = sql.length;
  while (i < n) {
    const c = sql[i];
    const two = sql.slice(i, i + 2);
    if (c === "'" || c === '"' || c === '`') {
      let j = i + 1;
      while (j < n) {
        if (sql[j] === c) {
          if (sql[j + 1] === c) j += 2; // a doubled quote
          else break;
        } else j++;
      }
      out += sql.slice(i, j + 1);
      i = j + 1;
    } else if (c === '[') {
      const j = sql.indexOf(']', i);
      const end = j < 0 ? n : j + 1;
      out += sql.slice(i, end);
      i = end;
    } else if (two === '--') {
      const j = sql.indexOf('\n', i);
      const end = j < 0 ? n : j;
      out += sql.slice(i, end);
      i = end;
    } else if (two === '/*') {
      const j = sql.indexOf('*/', i + 2);
      const end = j < 0 ? n : j + 2;
      out += sql.slice(i, end);
      i = end;
    } else if ((c === ':' || c === '@' || c === '$') && /[A-Za-z_]/.test(sql[i + 1] ?? '') && sql[i - 1] !== ':') {
      const m = /^[A-Za-z_][A-Za-z0-9_]*/.exec(sql.slice(i + 1))!;
      let k = names.indexOf(m[0]);
      if (k < 0) k = names.push(m[0]) - 1;
      out += '?' + (k + 1);
      i += 1 + m[0].length;
    } else {
      out += c;
      i++;
    }
  }
  return { sql: out, names };
}
