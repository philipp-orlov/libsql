import {
  ColumnDraft,
  addColumnSql,
  createIndexSql,
  createTableSql,
  dropColumnSql,
  dropObjectSql,
  emptyColumn,
  quoteIdentifier,
  renameColumnSql,
  renameTableSql,
} from './sql-builder';

function column(overrides: Partial<ColumnDraft>): ColumnDraft {
  return { ...emptyColumn(), ...overrides };
}

describe('sql-builder', () => {
  it('quotes an identifier and doubles a quote inside it', () => {
    expect(quoteIdentifier('person')).toBe('"person"');
    expect(quoteIdentifier('a "b" c')).toBe('"a ""b"" c"');
    expect(quoteIdentifier('x; DROP TABLE y')).toBe('"x; DROP TABLE y"');
  });

  describe('createTableSql', () => {
    it('writes a plain table', () => {
      const sql = createTableSql({
        name: 'person',
        columns: [column({ name: 'name', type: 'TEXT', notNull: true }), column({ name: 'age', type: 'INTEGER', defaultValue: '0' })],
        withoutRowid: false,
        strict: false,
      });
      expect(sql).toBe('CREATE TABLE "person" (\n\t"name" TEXT NOT NULL,\n\t"age" INTEGER DEFAULT 0\n);');
    });

    it('puts AUTOINCREMENT on the column of a lone primary key', () => {
      const sql = createTableSql({
        name: 't',
        columns: [column({ name: 'id', type: 'INTEGER', primaryKey: true, autoIncrement: true, notNull: true })],
        withoutRowid: false,
        strict: false,
      });
      expect(sql).toBe('CREATE TABLE "t" (\n\t"id" INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL\n);');
    });

    it('lists a primary key that has no AUTOINCREMENT as a table constraint, also a compound one', () => {
      const single = createTableSql({ name: 't', columns: [column({ name: 'id', type: 'INTEGER', primaryKey: true })], withoutRowid: false, strict: false });
      expect(single).toContain('\tPRIMARY KEY("id")');
      const compound = createTableSql({
        name: 't',
        columns: [column({ name: 'a', primaryKey: true }), column({ name: 'b', primaryKey: true, autoIncrement: true })],
        withoutRowid: false,
        strict: false,
      });
      expect(compound).toContain('\tPRIMARY KEY("a", "b")');
      expect(compound).not.toContain('AUTOINCREMENT');
    });

    it('adds the table options and the column flags', () => {
      const sql = createTableSql({
        name: 't',
        columns: [column({ name: 'k', type: 'TEXT', primaryKey: true }), column({ name: 'u', type: '', unique: true })],
        withoutRowid: true,
        strict: true,
      });
      expect(sql).toBe('CREATE TABLE "t" (\n\t"k" TEXT,\n\t"u" UNIQUE,\n\tPRIMARY KEY("k")\n) WITHOUT ROWID, STRICT;');
    });

    it('quotes the names it is given', () => {
      const sql = createTableSql({ name: 'a"b', columns: [column({ name: 'c d' })], withoutRowid: false, strict: false });
      expect(sql).toContain('CREATE TABLE "a""b"');
      expect(sql).toContain('"c d" TEXT');
    });
  });

  it('writes an index', () => {
    expect(createIndexSql({ name: 'i', table: 't', unique: false, columns: [{ name: 'a', descending: false }] })).toBe('CREATE INDEX "i" ON "t" ("a");');
    expect(
      createIndexSql({ name: 'i', table: 't', unique: true, columns: [{ name: 'a', descending: true }, { name: 'b', descending: false }] }),
    ).toBe('CREATE UNIQUE INDEX "i" ON "t" ("a" DESC, "b");');
  });

  it('writes the ALTER TABLE statements', () => {
    expect(renameTableSql('a', 'b')).toBe('ALTER TABLE "a" RENAME TO "b";');
    expect(renameColumnSql('t', 'x', 'y')).toBe('ALTER TABLE "t" RENAME COLUMN "x" TO "y";');
    expect(dropColumnSql('t', 'x')).toBe('ALTER TABLE "t" DROP COLUMN "x";');
    expect(addColumnSql('t', column({ name: 'z', type: 'REAL', notNull: true, defaultValue: '0' }))).toBe('ALTER TABLE "t" ADD COLUMN "z" REAL NOT NULL DEFAULT 0;');
  });

  it('writes DROP for every kind of object', () => {
    expect(dropObjectSql('table', 'a')).toBe('DROP TABLE "a";');
    expect(dropObjectSql('view', 'a')).toBe('DROP VIEW "a";');
    expect(dropObjectSql('index', 'a')).toBe('DROP INDEX "a";');
    expect(dropObjectSql('trigger', 'a')).toBe('DROP TRIGGER "a";');
  });
});
