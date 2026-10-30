// The statements the structure dialogs write. The dialogs show them before they run.

export function quoteIdentifier(name: string): string {
  return `"${name.replace(/"/g, '""')}"`;
}

export interface ColumnDraft {
  name: string;
  type: string;
  notNull: boolean;
  primaryKey: boolean;
  /** Only meaningful for a lone INTEGER primary key. */
  autoIncrement: boolean;
  unique: boolean;
  /** An SQL expression, as it would follow DEFAULT. */
  defaultValue: string;
}

export interface TableDraft {
  name: string;
  columns: ColumnDraft[];
  withoutRowid: boolean;
  strict: boolean;
}

export interface IndexDraft {
  name: string;
  table: string;
  unique: boolean;
  columns: { name: string; descending: boolean }[];
}

export function emptyColumn(): ColumnDraft {
  return { name: '', type: 'TEXT', notNull: false, primaryKey: false, autoIncrement: false, unique: false, defaultValue: '' };
}

function columnDefinition(column: ColumnDraft, inlineKey: boolean): string {
  const parts = [quoteIdentifier(column.name)];
  if (column.type.trim()) {
    parts.push(column.type.trim());
  }
  if (inlineKey) {
    parts.push('PRIMARY KEY');
    if (column.autoIncrement) {
      parts.push('AUTOINCREMENT');
    }
  }
  if (column.notNull) {
    parts.push('NOT NULL');
  }
  if (column.unique) {
    parts.push('UNIQUE');
  }
  if (column.defaultValue.trim()) {
    parts.push(`DEFAULT ${column.defaultValue.trim()}`);
  }
  return parts.join(' ');
}

export function createTableSql(draft: TableDraft): string {
  const keys = draft.columns.filter(column => column.primaryKey);
  // AUTOINCREMENT only has a form on the column itself.
  const inlineKey = keys.length === 1 && keys[0].autoIncrement ? keys[0] : null;
  const lines = draft.columns.map(column => '\t' + columnDefinition(column, column === inlineKey));
  if (keys.length > 0 && !inlineKey) {
    lines.push(`\tPRIMARY KEY(${keys.map(column => quoteIdentifier(column.name)).join(', ')})`);
  }
  const options = [draft.withoutRowid ? 'WITHOUT ROWID' : '', draft.strict ? 'STRICT' : ''].filter(Boolean);
  const tail = options.length > 0 ? ' ' + options.join(', ') : '';
  return `CREATE TABLE ${quoteIdentifier(draft.name)} (\n${lines.join(',\n')}\n)${tail};`;
}

export function createIndexSql(draft: IndexDraft): string {
  const columns = draft.columns.map(column => quoteIdentifier(column.name) + (column.descending ? ' DESC' : ''));
  return `CREATE ${draft.unique ? 'UNIQUE ' : ''}INDEX ${quoteIdentifier(draft.name)} ON ${quoteIdentifier(draft.table)} (${columns.join(', ')});`;
}

export function renameTableSql(table: string, newName: string): string {
  return `ALTER TABLE ${quoteIdentifier(table)} RENAME TO ${quoteIdentifier(newName)};`;
}

export function addColumnSql(table: string, column: ColumnDraft): string {
  return `ALTER TABLE ${quoteIdentifier(table)} ADD COLUMN ${columnDefinition(column, false)};`;
}

export function renameColumnSql(table: string, column: string, newName: string): string {
  return `ALTER TABLE ${quoteIdentifier(table)} RENAME COLUMN ${quoteIdentifier(column)} TO ${quoteIdentifier(newName)};`;
}

export function dropColumnSql(table: string, column: string): string {
  return `ALTER TABLE ${quoteIdentifier(table)} DROP COLUMN ${quoteIdentifier(column)};`;
}

export type ObjectKind = 'table' | 'view' | 'index' | 'trigger';

export function dropObjectSql(kind: ObjectKind, name: string): string {
  return `DROP ${kind.toUpperCase()} ${quoteIdentifier(name)};`;
}
