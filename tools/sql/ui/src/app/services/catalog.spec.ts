import { Cell } from '../models/api.models';
import { Catalog, ParamType, ParamValue, Reply, SocketResult } from '../remote';
import * as catalog from './catalog';

/** A result with these rows, in the shape the functions read (the part of ResultSetView they use). */
function result(columns: string[], rows: Cell[][], done: Partial<Reply> = {}): SocketResult {
  return {
    view: { columns: columns.length ? columns : (rows[0] ?? []).map(() => ''), rowCount: rows.length, cell: (r: number, c: number) => rows[r][c] } as never,
    done: { changes: 0, inTransaction: false, sql: '', strict: false, withoutRowid: false, ...done } as Reply,
  };
}

interface Call {
  sql: string;
  params?: ParamValue[];
}

/** A connection that answers from a script, in order of the statements it recognises, and records every call. */
class FakeConn implements catalog.Conn {
  calls: Call[] = [];
  describes: [Catalog, string | undefined][] = [];
  constructor(
    private readonly onExec: (sql: string, params?: ParamValue[]) => SocketResult = () => result([], []),
    private readonly onDescribe: (what: Catalog, name?: string) => SocketResult = () => result([], []),
  ) {}
  async exec(sql: string, params?: ParamValue[]): Promise<SocketResult> {
    this.calls.push({ sql, params });
    return this.onExec(sql, params);
  }
  async describe(what: Catalog, name?: string): Promise<SocketResult> {
    this.describes.push([what, name]);
    return this.onDescribe(what, name);
  }
}

const COLUMN_NAMES = ['name', 'type', 'declared_type', 'affinity', 'collation', 'primary_key', 'not_null', 'unique', 'has_default', 'default_value', 'default_sql', 'generated', 'pk_position'];
/** A Describe Table row. */
const col = (name: string, declared: string, affinity: string, key = 0, notNull = 0, position = 0): Cell[] => [
  name, 0, declared, affinity, 'BINARY', key ? 1 : 0, notNull, 0, 0, null, '', 0, position,
];

const text = (value: string): ParamValue => ({ type: ParamType.Text, value });
const int = (value: number | bigint): ParamValue => ({ type: ParamType.Integer, value });

/** A schema with the table person(id INTEGER PRIMARY KEY, name TEXT NOT NULL) and the view adults. */
function personConn(overrides: { withoutRowid?: boolean; changes?: number } = {}): FakeConn {
  return new FakeConn(
    (sql, params) => {
      if (sql.startsWith('SELECT type, name FROM sqlite_schema')) {
        const wanted = (params![0] as { value: string }).value.toLowerCase();
        return wanted === 'adults' ? result(['type', 'name'], [['view', 'adults']]) : wanted === 'person' ? result(['type', 'name'], [['table', 'person']]) : result(['type', 'name'], []);
      }
      if (sql.startsWith('PRAGMA table_info')) {
        return result([], [[0, 'id', 'INTEGER', 0, null, 0], [1, 'name', 'TEXT', 1, null, 0]]);
      }
      if (sql.startsWith('SELECT count(*)')) {
        return result(['count'], [[7]]);
      }
      if (sql.startsWith('SELECT')) {
        return result([], [[1, 1, 'Ada'], [2, 2, 'Bob']]);
      }
      return result([], [], { changes: overrides.changes ?? 1 });
    },
    () => result(COLUMN_NAMES, [col('id', 'INTEGER', 'INTEGER', 1, 0, 1), col('name', 'TEXT', 'TEXT', 0, 1)], { sql: 'CREATE TABLE person(...)', withoutRowid: !!overrides.withoutRowid, strict: true }),
  );
}

describe('catalog', () => {
  describe('cells as parameters', () => {
    it('sends integers, reals, text and null as their own types', () => {
      expect(catalog.toParam(null)).toBeNull();
      expect(catalog.toParam(5)).toEqual(int(5));
      expect(catalog.toParam(1.5)).toEqual({ type: ParamType.Real, value: 1.5 });
      expect(catalog.toParam('x')).toEqual(text('x'));
      expect(catalog.toParam({ type: 'integer', value: '9223372036854775807' })).toEqual(int(9223372036854775807n));
    });

    it('does not send a blob preview back', () => {
      expect(() => catalog.toParam({ type: 'blob', size: 3, value: '616263' })).toThrowError(/cannot be sent back/);
    });

    it('reads a filter that looks like a number as one', () => {
      expect(catalog.guessParam('42')).toEqual(int(42n));
      expect(catalog.guessParam('-7')).toEqual(int(-7n));
      expect(catalog.guessParam('2.5')).toEqual({ type: ParamType.Real, value: 2.5 });
      expect(catalog.guessParam('1e3')).toEqual({ type: ParamType.Real, value: 1000 });
      expect(catalog.guessParam('abc')).toEqual(text('abc'));
      expect(catalog.guessParam('12abc')).toEqual(text('12abc'));
      expect(catalog.guessParam('')).toEqual(text(''));
      expect(catalog.guessParam('0x10')).toEqual(text('0x10'));
    });
  });

  describe('filters', () => {
    it('ignores an empty filter', () => {
      expect(catalog.filterCondition('a', '   ')).toBeNull();
    });
    it('reads the operators, longest first', () => {
      expect(catalog.filterCondition('a', '<>5')).toEqual({ sql: '"a" <> ?', params: [int(5n)] });
      expect(catalog.filterCondition('a', '!=5')?.sql).toBe('"a" <> ?');
      expect(catalog.filterCondition('a', '>= 5')).toEqual({ sql: '"a" >= ?', params: [int(5n)] });
      expect(catalog.filterCondition('a', '<5')?.sql).toBe('"a" < ?');
      expect(catalog.filterCondition('a', '==x')).toEqual({ sql: '"a" = ?', params: [text('x')] });
      expect(catalog.filterCondition('a', '=x')?.sql).toBe('"a" = ?');
    });
    it('compares with NULL by IS', () => {
      expect(catalog.filterCondition('a', '=null')).toEqual({ sql: '"a" IS NULL', params: [] });
      expect(catalog.filterCondition('a', '<> NULL')).toEqual({ sql: '"a" IS NOT NULL', params: [] });
    });
    it('looks for plain text anywhere, ignoring case', () => {
      expect(catalog.filterCondition('na"me', 'Ad')).toEqual({ sql: 'instr(lower(CAST("na""me" AS TEXT)), lower(?)) > 0', params: [text('Ad')] });
    });
  });

  describe('browse', () => {
    it('counts and pages with the sort and the filters, keys first', async () => {
      const conn = personConn();
      const page = await catalog.browse(conn, { table: 'PERSON', offset: 20, limit: 10, sort: [{ column: 'Name', descending: true }], filters: { name: 'a', id: '>1' } }, false);
      const [, count, select] = conn.calls;
      expect(count.sql).toBe('SELECT count(*) FROM "person" WHERE instr(lower(CAST("name" AS TEXT)), lower(?)) > 0 AND "id" > ?');
      expect(count.params).toEqual([text('a'), int(1n)]);
      expect(select.sql).toBe('SELECT rowid, "id", "name" FROM "person" WHERE instr(lower(CAST("name" AS TEXT)), lower(?)) > 0 AND "id" > ? ORDER BY "name" DESC LIMIT 10 OFFSET 20');
      expect(page.table).toBe('person');
      expect(page.total).toBe(7);
      expect(page.editable).toBeTrue();
      expect(page.keys).toEqual([[1], [2]]);
      expect(page.rows).toEqual([[1, 'Ada'], [2, 'Bob']]);
      expect(page.columns.map(c => c.name)).toEqual(['id', 'name']);
      expect(page.columns[0].primaryKey).toBeTrue();
      expect(page.columns[1].notNull).toBeTrue();
    });

    it('addresses the rows of a WITHOUT ROWID table by its key', async () => {
      const conn = personConn({ withoutRowid: true });
      const page = await catalog.browse(conn, { table: 'person', offset: 0, limit: 100, sort: [], filters: {} }, false);
      expect(conn.calls.at(-1)!.sql).toBe('SELECT "id", "id", "name" FROM "person" LIMIT 100 OFFSET 0');
      expect(page.editable).toBeTrue();
    });

    it('does not offer to edit a view or a read-only database', async () => {
      const view = await catalog.browse(personConn(), { table: 'adults', offset: 0, limit: 5, sort: [], filters: {} }, false);
      expect(view.view).toBeTrue();
      expect(view.editable).toBeFalse();
      expect(view.keys).toEqual([[], []]);
      const readOnly = await catalog.browse(personConn(), { table: 'person', offset: 0, limit: 5, sort: [], filters: {} }, true);
      expect(readOnly.editable).toBeFalse();
    });

    it('limits the page and refuses what is not there', async () => {
      const conn = personConn();
      await catalog.browse(conn, { table: 'person', offset: -4, limit: 100000, sort: [], filters: {} }, false);
      expect(conn.calls.at(-1)!.sql).toContain('LIMIT 1000 OFFSET 0');
      await expectAsync(catalog.browse(conn, { table: 'nope', offset: 0, limit: 5, sort: [], filters: {} }, false)).toBeRejectedWithError(/no such table: nope/);
      await expectAsync(catalog.browse(conn, { table: 'person', offset: 0, limit: 5, sort: [], filters: { zzz: 'x' } }, false)).toBeRejectedWithError(/no such column: zzz/);
      await expectAsync(catalog.browse(conn, { table: 'person', offset: 0, limit: 5, sort: [{ column: 'zzz', descending: false }], filters: {} }, false)).toBeRejectedWithError(/no such column: zzz/);
    });
  });

  describe('edits', () => {
    it('inserts the named columns, or the defaults', async () => {
      const conn = personConn();
      await catalog.insertRow(conn, 'person', { name: 'Cy', id: 3 }, false);
      expect(conn.calls.at(-1)).toEqual({ sql: 'INSERT INTO "person" ("name", "id") VALUES (?, ?)', params: [text('Cy'), int(3)] });
      await catalog.insertRow(conn, 'person', {}, false);
      expect(conn.calls.at(-1)!.sql).toBe('INSERT INTO "person" DEFAULT VALUES');
    });

    it('updates the row its key names, and says when it is gone', async () => {
      const conn = personConn();
      await catalog.updateRow(conn, 'person', [2], { name: 'Bo' }, false);
      expect(conn.calls.at(-1)).toEqual({ sql: 'UPDATE "person" SET "name" = ?1 WHERE rowid = ?2', params: [text('Bo'), int(2)] });
      await expectAsync(catalog.updateRow(personConn({ changes: 0 }), 'person', [9], { name: 'x' }, false)).toBeRejectedWithError(/no longer exists/);
    });

    it('deletes each key', async () => {
      const conn = personConn();
      await catalog.deleteRows(conn, 'person', [[1], [2]], false);
      const deletes = conn.calls.filter(c => c.sql.startsWith('DELETE'));
      expect(deletes.map(c => c.params)).toEqual([[int(1)], [int(2)]]);
      expect(deletes[0].sql).toBe('DELETE FROM "person" WHERE rowid = ?1');
    });

    it('refuses to edit a view, a read-only database, or a wrong key', async () => {
      await expectAsync(catalog.insertRow(personConn(), 'adults', { name: 'x' }, false)).toBeRejectedWithError(/is a view/);
      await expectAsync(catalog.insertRow(personConn(), 'person', { name: 'x' }, true)).toBeRejectedWithError(/read-only/);
      await expectAsync(catalog.updateRow(personConn(), 'person', [1, 2], { name: 'x' }, false)).toBeRejectedWithError(/array of 1 cells/);
      await expectAsync(catalog.updateRow(personConn(), 'person', [1], {}, false)).toBeRejectedWithError(/at least one column/);
      await expectAsync(catalog.updateRow(personConn(), 'person', [1], { zzz: 1 }, false)).toBeRejectedWithError(/no such column/);
    });
  });

  describe('schema', () => {
    it('collects tables, views, triggers and the columns of every index', async () => {
      const conn = new FakeConn(
        sql => {
          if (sql.startsWith('SELECT type, name, tbl_name')) {
            return result([], [['table', 'person', 'person', 'CREATE TABLE person(...)'], ['view', 'adults', 'adults', 'CREATE VIEW adults AS ...'], ['trigger', 'trg', 'person', 'CREATE TRIGGER trg ...'], ['index', 'ix', 'person', 'CREATE INDEX ix ...']]);
          }
          return result([], [[0, 'id', 'INTEGER', 1, null, 0]]);
        },
        what =>
          what === Catalog.Indexes
            ? result([], [['ix', 'person', 1, 0, 'CREATE UNIQUE INDEX ix ON person(a, b)', 0, 'a'], ['ix', 'person', 1, 0, 'CREATE UNIQUE INDEX ix ON person(a, b)', 1, 'b'], ['sqlite_autoindex_person_1', 'person', 1, 0, '', 0, 'id']])
            : result(COLUMN_NAMES, [col('id', 'INTEGER', 'INTEGER', 1, 0, 1)], { sql: 'CREATE TABLE person(...)', strict: true }),
      );
      const schema = await catalog.loadSchema(conn);
      expect(schema.tables.map(t => [t.name, t.strict, t.withoutRowid, t.columns.length])).toEqual([['person', true, false, 1]]);
      expect(schema.tables[0].sql).toBe('CREATE TABLE person(...)');
      expect(schema.views.map(v => [v.name, v.columns.map(c => c.name)])).toEqual([['adults', ['id']]]);
      expect(schema.triggers).toEqual([{ name: 'trg', table: 'person', sql: 'CREATE TRIGGER trg ...' }]);
      expect(schema.indexes).toEqual([{ name: 'ix', table: 'person', unique: true, partial: false, sql: 'CREATE UNIQUE INDEX ix ON person(a, b)', columns: ['a', 'b'] }]);
    });
  });

  describe('dump', () => {
    it('writes the schema, the rows and then the other objects', async () => {
      const conn = new FakeConn(
        sql => {
          if (sql.startsWith("SELECT name, sql FROM sqlite_schema WHERE type = 'table'")) {
            return result([], [['t', 'CREATE TABLE t(a, b)']]);
          }
          if (sql.startsWith('SELECT quote(')) {
            return result([], [["1,'x'"], ['NULL,2']]);
          }
          return result([], [['CREATE INDEX i ON t(a)']]);
        },
        () => result(COLUMN_NAMES, [col('a', '', 'BLOB'), col('b', '', 'BLOB')]),
      );
      expect(await catalog.dump(conn)).toBe(
        "BEGIN TRANSACTION;\nCREATE TABLE t(a, b);\nINSERT INTO \"t\"(\"a\",\"b\") VALUES(1,'x');\nINSERT INTO \"t\"(\"a\",\"b\") VALUES(NULL,2);\nCREATE INDEX i ON t(a);\nCOMMIT;\n",
      );
      expect(conn.calls[1].sql).toBe('SELECT quote("a")||\',\'||quote("b") FROM "t"');
    });
  });

  describe('pragmas', () => {
    it('lists the known pragmas with their values, read-only when the database is', async () => {
      const conn = new FakeConn(sql => (sql === 'PRAGMA encoding' ? result([], []) : result([], [[1]])));
      const list = await catalog.loadPragmas(conn, true);
      expect(list.some(p => p.name === 'encoding')).toBeFalse(); // reported nothing
      expect(list.find(p => p.name === 'foreign_keys')).toEqual(jasmine.objectContaining({ kind: 'boolean', scope: 'session', readOnly: true, value: 1 }));
      expect(list.find(p => p.name === 'temp_store')!.choices.length).toBe(3);
    });

    it('sets a pragma only when it is allowed', async () => {
      const conn = new FakeConn();
      await catalog.setPragma(conn, 'foreign_keys', 1, false, false);
      expect(conn.calls.at(-1)!.sql).toBe('PRAGMA foreign_keys = 1');
      await expectAsync(catalog.setPragma(conn, 'page_size', 1, false, false)).toBeRejectedWithError(/cannot be changed/);
      await expectAsync(catalog.setPragma(conn, 'nope', 1, false, false)).toBeRejectedWithError(/cannot be changed/);
      await expectAsync(catalog.setPragma(conn, 'foreign_keys', 1, true, false)).toBeRejectedWithError(/read-only/);
      await expectAsync(catalog.setPragma(conn, 'foreign_keys', 1, false, true)).toBeRejectedWithError(/pending changes/);
      await expectAsync(catalog.setPragma(conn, 'foreign_keys', 2, false, false)).toBeRejectedWithError(/takes 0 or 1/);
      await expectAsync(catalog.setPragma(conn, 'temp_store', 7, false, false)).toBeRejectedWithError(/does not take 7/);
      await expectAsync(catalog.setPragma(conn, 'busy_timeout', 1.5, false, false)).toBeRejectedWithError(/must be an integer/);
    });
  });

  describe('facts', () => {
    it('reads the function names and the path', async () => {
      const conn = new FakeConn(sql => (sql.includes('pragma_function_list') ? result([], [['abs'], ['count']]) : result([], [['/data/x.db']])));
      expect(await catalog.loadFacts(conn)).toEqual({ path: '/data/x.db', functions: ['abs', 'count'] });
    });
  });
});
