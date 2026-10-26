// What the tabs need from the database, asked over the SQL WebSocket route: the schema, a page of a table with the
// user's sort and filters, row edits, the pragmas, a dump. Every function takes the connection it talks through
// (the part of SqlSocket it uses), so a test can give it a fake. The SQL is the one the JSON API used to run on the
// server (tools/sql/src/web_api.cpp), moved here when the page began to use the socket for everything.

import { BrowseReply, BrowseRequest, Cell, ColumnInfo, IndexInfo, IntegerCell, PragmaInfo, Schema, TableInfo, TriggerInfo, ViewInfo } from '../models/api.models';
import { Catalog, ParamType } from '../remote';
import type { ParamValue, ResultOptionsFields, SocketResult } from '../remote';
import { quoteIdentifier } from '../utils/sql-builder';

/** The part of SqlSocket used here. */
export interface Conn {
  exec(sql: string, params?: ParamValue[], options?: ResultOptionsFields): Promise<SocketResult>;
  describe(what: Catalog, name?: string): Promise<SocketResult>;
}

/** A request the user can fix (a bad table name, a view that cannot be edited). */
export class CatalogError extends Error {}

/** Blob cells carry their size and this many leading bytes. */
export const BLOB_PREVIEW = 32;
export const DEFAULT_PAGE_ROWS = 100;
export const MAX_PAGE_ROWS = 1000;
/** Rows an Execute SQL run keeps. */
export const MAX_RESULT_ROWS = 5000;

export const RESULT_OPTIONS: ResultOptionsFields = { blobPrefix: BLOB_PREVIEW, countAllRows: true, maxRows: MAX_RESULT_ROWS };

// ------------------------------------------------------------------------------------------------- cells ---

export function rowsOf(result: SocketResult): Cell[][] {
  const view = result.view;
  if (!view) {
    return [];
  }
  const rows: Cell[][] = [];
  for (let r = 0; r < view.rowCount; r++) {
    const row: Cell[] = [];
    for (let c = 0; c < view.columns.length; c++) {
      row.push(view.cell(r, c));
    }
    rows.push(row);
  }
  return rows;
}

export function text(cell: Cell | undefined): string {
  if (cell === null || cell === undefined) {
    return '';
  }
  return typeof cell === 'object' ? (cell.type === 'blob' ? '' : cell.value) : String(cell);
}

export function int(cell: Cell | undefined): number {
  if (cell === null || cell === undefined) {
    return 0;
  }
  if (typeof cell === 'object') {
    return cell.type === 'blob' ? 0 : Number(cell.value);
  }
  return Number(cell);
}

/** A cell as the value to bind. A blob cell holds only a preview, so it cannot be sent back. */
export function toParam(cell: Cell): ParamValue {
  if (cell === null) {
    return null;
  }
  if (typeof cell === 'number') {
    return Number.isInteger(cell) ? { type: ParamType.Integer, value: cell } : { type: ParamType.Real, value: cell };
  }
  if (typeof cell === 'string') {
    return { type: ParamType.Text, value: cell };
  }
  if (cell.type === 'integer') {
    return { type: ParamType.Integer, value: BigInt((cell as IntegerCell).value) };
  }
  throw new CatalogError('this cell cannot be sent back to the database');
}

/** A filter text that reads as a number is compared as one, the way a typed column would. */
export function guessParam(value: string): ParamValue {
  if (/^[0-9+\-.]/.test(value)) {
    if (/^[+-]?\d+$/.test(value)) {
      const big = BigInt(value);
      if (big >= -(2n ** 63n) && big < 2n ** 63n) {
        return { type: ParamType.Integer, value: big };
      }
    }
    const real = Number(value);
    if (value.trim() === value && Number.isFinite(real) && /^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/.test(value)) {
      return { type: ParamType.Real, value: real };
    }
  }
  return { type: ParamType.Text, value };
}

// ---------------------------------------------------------------------------------------------- schema ---

function columnOf(row: Cell[]): ColumnInfo {
  // name, type, declared_type, affinity, collation, primary_key, not_null, unique, has_default, default_value,
  // default_sql, generated, pk_position
  const hasDefault = int(row[8]) !== 0;
  return {
    name: text(row[0]),
    type: text(row[2]),
    affinity: text(row[3]),
    primaryKey: int(row[5]) !== 0,
    notNull: int(row[6]) !== 0,
    unique: int(row[7]) !== 0,
    generated: int(row[11]),
    defaultSql: hasDefault ? text(row[10]) : null,
  };
}

/** The columns of a table, in order, with its CREATE statement and the positions of its primary key columns. */
export async function describeTable(conn: Conn, name: string): Promise<TableInfo & { primaryKey: string[] }> {
  const result = await conn.describe(Catalog.Table, name);
  const rows = rowsOf(result);
  const keyed = rows.filter(row => int(row[12]) > 0).sort((a, b) => int(a[12]) - int(b[12]));
  return {
    name,
    sql: result.done.sql,
    strict: result.done.strict,
    withoutRowid: result.done.withoutRowid,
    columns: rows.map(columnOf),
    primaryKey: keyed.map(row => text(row[0])),
  };
}

async function viewColumns(conn: Conn, name: string): Promise<ColumnInfo[]> {
  const rows = rowsOf(await conn.exec(`PRAGMA table_info(${quoteIdentifier(name)})`));
  // cid, name, type, notnull, dflt_value, pk
  return rows.map(row => ({
    name: text(row[1]),
    type: text(row[2]),
    affinity: '',
    primaryKey: false,
    notNull: int(row[3]) !== 0,
    unique: false,
    generated: 0,
    defaultSql: null,
  }));
}

export async function loadSchema(conn: Conn): Promise<Schema> {
  const objects = rowsOf(await conn.exec('SELECT type, name, tbl_name, sql FROM sqlite_schema WHERE sql IS NOT NULL'));
  const tables: TableInfo[] = [];
  const views: ViewInfo[] = [];
  const triggers: TriggerInfo[] = [];
  for (const [type, name, table, sql] of objects) {
    if (type === 'table') {
      const { primaryKey: _unused, ...info } = await describeTable(conn, text(name));
      tables.push({ ...info, sql: text(sql) });
    } else if (type === 'view') {
      views.push({ name: text(name), sql: text(sql), columns: await viewColumns(conn, text(name)) });
    } else if (type === 'trigger') {
      triggers.push({ name: text(name), table: text(table), sql: text(sql) });
    }
  }
  // index_name, table_name, unique, partial, sql, position, column: one row per column
  const indexes: IndexInfo[] = [];
  for (const row of rowsOf(await conn.describe(Catalog.Indexes, ''))) {
    const last = indexes[indexes.length - 1];
    if (last && last.name === text(row[0])) {
      last.columns.push(text(row[6]));
    } else if (text(row[4]) !== '') {
      indexes.push({ name: text(row[0]), table: text(row[1]), unique: int(row[2]) !== 0, partial: int(row[3]) !== 0, sql: text(row[4]), columns: [text(row[6])] });
    }
  }
  return { tables, views, indexes, triggers };
}

/** A script that rebuilds the schema and the rows, as `.dump` of the command line does. */
export async function dump(conn: Conn): Promise<string> {
  let script = 'BEGIN TRANSACTION;\n';
  const tables = rowsOf(await conn.exec("SELECT name, sql FROM sqlite_schema WHERE type = 'table' AND sql IS NOT NULL"));
  for (const [nameCell, sqlCell] of tables) {
    const name = text(nameCell);
    script += `${text(sqlCell)};\n`;
    const columns: string[] = [];
    const quoted: string[] = [];
    for (const column of (await describeTable(conn, name)).columns) {
      if (column.generated) {
        continue;
      }
      columns.push(quoteIdentifier(column.name));
      quoted.push(`quote(${quoteIdentifier(column.name)})`);
    }
    if (columns.length === 0) {
      continue;
    }
    const prefix = `INSERT INTO ${quoteIdentifier(name)}(${columns.join(',')}) VALUES(`;
    for (const [values] of rowsOf(await conn.exec(`SELECT ${quoted.join("||','||")} FROM ${quoteIdentifier(name)}`))) {
      script += `${prefix}${text(values)});\n`;
    }
  }
  for (const [sql] of rowsOf(await conn.exec("SELECT sql FROM sqlite_schema WHERE type <> 'table' AND sql IS NOT NULL"))) {
    script += `${text(sql)};\n`;
  }
  return script + 'COMMIT;\n';
}

// ---------------------------------------------------------------------------------------------- browse ---

/** A table or view, as far as browsing and editing need to know it. */
export interface Relation {
  name: string;
  view: boolean;
  columns: ColumnInfo[];
  /** SQL expressions that address one row: `rowid`, or the PRIMARY KEY of a WITHOUT ROWID table. Empty: a row cannot be addressed. */
  keys: string[];
}

export function findColumn(relation: Relation, wanted: string): ColumnInfo | undefined {
  const lower = wanted.toLowerCase();
  return relation.columns.find(column => column.name.toLowerCase() === lower);
}

export async function describeRelation(conn: Conn, wanted: string): Promise<Relation> {
  const found = rowsOf(
    await conn.exec("SELECT type, name FROM sqlite_schema WHERE type IN ('table', 'view') AND name = ?1 COLLATE NOCASE", [{ type: ParamType.Text, value: wanted }]),
  );
  if (found.length === 0) {
    throw new CatalogError(`no such table: ${wanted}`);
  }
  const name = text(found[0][1]);
  if (text(found[0][0]) === 'view') {
    return { name, view: true, columns: await viewColumns(conn, name), keys: [] };
  }
  const info = await describeTable(conn, name);
  const relation: Relation = { name, view: false, columns: info.columns, keys: [] };
  if (info.withoutRowid) {
    relation.keys = info.primaryKey.map(quoteIdentifier);
  } else if (!findColumn(relation, 'rowid')) {
    relation.keys = ['rowid'];
  }
  return relation;
}

export interface Condition {
  sql: string;
  params: ParamValue[];
}

/** `typed` is what the user typed under a column heading: `>5`, `=abc`, `<>NULL`, or plain text to look for. */
export function filterCondition(column: string, typed: string): Condition | null {
  const entered = typed.trim();
  if (entered === '') {
    return null;
  }
  const id = quoteIdentifier(column);
  // Longer operators first, so that `<>` is not read as `<`.
  const operators: [string, string][] = [['<>', '<>'], ['!=', '<>'], ['>=', '>='], ['<=', '<='], ['==', '='], ['=', '='], ['>', '>'], ['<', '<']];
  for (const [prefix, op] of operators) {
    if (!entered.startsWith(prefix)) {
      continue;
    }
    const value = entered.slice(prefix.length).trim();
    if (value.toLowerCase() === 'null' && (op === '=' || op === '<>')) {
      return { sql: `${id} ${op === '=' ? 'IS NULL' : 'IS NOT NULL'}`, params: [] };
    }
    return { sql: `${id} ${op} ?`, params: [guessParam(value)] };
  }
  return { sql: `instr(lower(CAST(${id} AS TEXT)), lower(?)) > 0`, params: [{ type: ParamType.Text, value: entered }] };
}

export async function browse(conn: Conn, request: BrowseRequest, readOnly: boolean): Promise<BrowseReply> {
  const relation = await describeRelation(conn, request.table);
  const offset = Math.max(0, Math.floor(request.offset || 0));
  const limit = Math.min(MAX_PAGE_ROWS, Math.max(1, Math.floor(request.limit || DEFAULT_PAGE_ROWS)));

  let where = '';
  const params: ParamValue[] = [];
  for (const [name, typed] of Object.entries(request.filters ?? {})) {
    const column = findColumn(relation, name);
    if (!column) {
      throw new CatalogError(`no such column: ${name}`);
    }
    const condition = filterCondition(column.name, typed);
    if (condition) {
      where += (where ? ' AND ' : ' WHERE ') + condition.sql;
      params.push(...condition.params);
    }
  }
  let order = '';
  for (const entry of request.sort ?? []) {
    const column = findColumn(relation, entry.column);
    if (!column) {
      throw new CatalogError(`no such column: ${entry.column}`);
    }
    order += (order ? ', ' : ' ORDER BY ') + quoteIdentifier(column.name) + (entry.descending ? ' DESC' : '');
  }
  const selected = [...relation.keys, ...relation.columns.map(column => quoteIdentifier(column.name))].join(', ');
  const from = ` FROM ${quoteIdentifier(relation.name)}${where}`;
  const total = int(rowsOf(await conn.exec(`SELECT count(*)${from}`, params))[0]?.[0]);
  const page = rowsOf(await conn.exec(`SELECT ${selected}${from}${order} LIMIT ${limit} OFFSET ${offset}`, params, { blobPrefix: BLOB_PREVIEW }));

  const keyCount = relation.keys.length;
  return {
    inTransaction: false, // the caller knows better: it asks the connection
    table: relation.name,
    view: relation.view,
    editable: !relation.view && !readOnly && keyCount > 0,
    columns: relation.columns,
    rows: page.map(row => row.slice(keyCount)),
    keys: page.map(row => row.slice(0, keyCount)),
    total,
    offset,
    limit,
  };
}

// ---------------------------------------------------------------------------------------------- edits ---

function requireEditable(relation: Relation, readOnly: boolean): void {
  if (relation.view) {
    throw new CatalogError(`${relation.name} is a view and cannot be edited`);
  }
  if (readOnly) {
    throw new CatalogError('the database is open read-only');
  }
  if (relation.keys.length === 0) {
    throw new CatalogError(`the rows of ${relation.name} cannot be addressed`);
  }
}

/** `k1 = ?1 AND k2 = ?2` for the key expressions, with the key cells bound after `first` earlier parameters. */
function keyCondition(relation: Relation, first: number): string {
  return relation.keys.map((key, i) => `${key} = ?${first + i + 1}`).join(' AND ');
}

function keyParams(relation: Relation, key: Cell[]): ParamValue[] {
  if (!Array.isArray(key) || key.length !== relation.keys.length) {
    throw new CatalogError(`a row key must be an array of ${relation.keys.length} cells`);
  }
  return key.map(toParam);
}

export async function insertRow(conn: Conn, table: string, values: Record<string, Cell>, readOnly: boolean): Promise<void> {
  const relation = await describeRelation(conn, table);
  requireEditable(relation, readOnly);
  const names: string[] = [];
  const params: ParamValue[] = [];
  for (const [name, cell] of Object.entries(values)) {
    const column = findColumn(relation, name);
    if (!column) {
      throw new CatalogError(`no such column: ${name}`);
    }
    names.push(quoteIdentifier(column.name));
    params.push(toParam(cell));
  }
  const sql =
    `INSERT INTO ${quoteIdentifier(relation.name)}` +
    (names.length === 0 ? ' DEFAULT VALUES' : ` (${names.join(', ')}) VALUES (${names.map(() => '?').join(', ')})`);
  await conn.exec(sql, params);
}

export async function updateRow(conn: Conn, table: string, key: Cell[], set: Record<string, Cell>, readOnly: boolean): Promise<void> {
  const relation = await describeRelation(conn, table);
  requireEditable(relation, readOnly);
  const entries = Object.entries(set);
  if (entries.length === 0) {
    throw new CatalogError('"set" must name at least one column');
  }
  const params: ParamValue[] = [];
  const assignments = entries.map(([name, cell]) => {
    const column = findColumn(relation, name);
    if (!column) {
      throw new CatalogError(`no such column: ${name}`);
    }
    params.push(toParam(cell));
    return `${quoteIdentifier(column.name)} = ?${params.length}`;
  });
  const assigned = params.length;
  params.push(...keyParams(relation, key));
  const done = await conn.exec(`UPDATE ${quoteIdentifier(relation.name)} SET ${assignments.join(', ')} WHERE ${keyCondition(relation, assigned)}`, params);
  if (done.done.changes !== 1) {
    throw new CatalogError('the row no longer exists');
  }
}

export async function deleteRows(conn: Conn, table: string, keys: Cell[][], readOnly: boolean): Promise<void> {
  const relation = await describeRelation(conn, table);
  requireEditable(relation, readOnly);
  const sql = `DELETE FROM ${quoteIdentifier(relation.name)} WHERE ${keyCondition(relation, 0)}`;
  for (const key of keys) {
    await conn.exec(sql, keyParams(relation, key));
  }
}

// ---------------------------------------------------------------------------------------------- pragmas ---

interface PragmaSpec {
  name: string;
  kind: PragmaInfo['kind'];
  readOnly: boolean;
  scope: PragmaInfo['scope'];
  description: string;
  choices?: { value: number; label: string }[];
}

// Only pragmas that change what libsql does are listed.
export const PRAGMA_SPECS: PragmaSpec[] = [
  { name: 'foreign_keys', kind: 'boolean', readOnly: false, scope: 'session', description: 'Enforce FOREIGN KEY constraints.' },
  { name: 'recursive_triggers', kind: 'boolean', readOnly: false, scope: 'session', description: 'Let a trigger fire itself.' },
  { name: 'ignore_check_constraints', kind: 'boolean', readOnly: false, scope: 'session', description: 'Do not enforce CHECK constraints.' },
  {
    name: 'temp_store',
    kind: 'choice',
    readOnly: false,
    scope: 'session',
    description: 'Where a large ORDER BY may put its sorted runs. Memory never writes a temporary file.',
    choices: [{ value: 0, label: 'Default' }, { value: 1, label: 'File' }, { value: 2, label: 'Memory' }],
  },
  { name: 'busy_timeout', kind: 'integer', readOnly: false, scope: 'session', description: 'Milliseconds a write waits for another writer. 0 waits without a limit.' },
  { name: 'user_version', kind: 'integer', readOnly: false, scope: 'file', description: 'A number the application keeps in the file, for example a schema version.' },
  { name: 'application_id', kind: 'integer', readOnly: false, scope: 'file', description: 'A number that tells which application owns the file.' },
  {
    name: 'journal_mode',
    kind: 'text',
    readOnly: true,
    scope: 'store',
    description:
      'Always wal. libsql reports this name because SQLite tools ask for it. The store is copy-on-write: a commit writes new pages and then switches to them, so there is no journal and no WAL file.',
  },
  { name: 'encoding', kind: 'text', readOnly: true, scope: 'store', description: 'Text is always UTF-8.' },
  { name: 'page_size', kind: 'integer', readOnly: true, scope: 'store', description: 'Bytes in one page of the store.' },
  { name: 'page_count', kind: 'integer', readOnly: true, scope: 'store', description: 'Pages in use.' },
  { name: 'freelist_count', kind: 'integer', readOnly: true, scope: 'store', description: 'Pages free for reuse.' },
  { name: 'schema_version', kind: 'integer', readOnly: true, scope: 'store', description: 'Counts the changes to the schema.' },
];

export async function loadPragmas(conn: Conn, readOnly: boolean): Promise<PragmaInfo[]> {
  const list: PragmaInfo[] = [];
  for (const spec of PRAGMA_SPECS) {
    const rows = rowsOf(await conn.exec(`PRAGMA ${spec.name}`));
    if (rows.length === 0) {
      continue;
    }
    list.push({
      name: spec.name,
      kind: spec.kind,
      scope: spec.scope,
      description: spec.description,
      readOnly: spec.readOnly || readOnly,
      value: rows[0][0],
      choices: spec.choices ?? [],
    });
  }
  return list;
}

export async function setPragma(conn: Conn, name: string, value: number, readOnly: boolean, inTransaction: boolean): Promise<void> {
  const spec = PRAGMA_SPECS.find(s => s.name === name);
  if (!spec || spec.readOnly) {
    throw new CatalogError(`PRAGMA ${name} cannot be changed here`);
  }
  if (readOnly) {
    throw new CatalogError('the database is open read-only');
  }
  if (inTransaction) {
    throw new CatalogError('write or revert the pending changes before changing a PRAGMA');
  }
  if (!Number.isInteger(value)) {
    throw new CatalogError(`the value of PRAGMA ${name} must be an integer`);
  }
  if (spec.kind === 'boolean' && value !== 0 && value !== 1) {
    throw new CatalogError(`PRAGMA ${name} takes 0 or 1`);
  }
  if (spec.kind === 'choice' && !(spec.choices ?? []).some(choice => choice.value === value)) {
    throw new CatalogError(`PRAGMA ${name} does not take ${value}`);
  }
  await conn.exec(`PRAGMA ${name} = ${value}`);
}

// ------------------------------------------------------------------------------------------ the database ---

export interface DatabaseFacts {
  path: string;
  functions: string[];
}

/** Where the database lives and the function names the SQL editor offers beside the keywords. */
export async function loadFacts(conn: Conn): Promise<DatabaseFacts> {
  const functions = rowsOf(await conn.exec('SELECT DISTINCT name FROM pragma_function_list ORDER BY name')).map(row => text(row[0]));
  let path = '';
  try {
    path = text(rowsOf(await conn.exec("SELECT file FROM pragma_database_list WHERE name = 'main'"))[0]?.[0]);
  } catch {
    // a server that cannot say shows no path
  }
  return { path, functions };
}
