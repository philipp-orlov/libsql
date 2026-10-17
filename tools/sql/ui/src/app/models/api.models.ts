// The JSON the sql tool's web API speaks (tools/sql/include/web_api.hpp).

export interface IntegerCell {
  type: 'integer';
  /** An INTEGER a JavaScript number cannot hold exactly. */
  value: string;
}

export interface BlobCell {
  type: 'blob';
  size: number;
  /** Hex of the first bytes only. */
  value: string;
}

/** null, a number or a string stand for themselves. */
export type Cell = null | number | string | IntegerCell | BlobCell;

/** Every reply says whether changes are waiting for Write Changes. */
export interface Pending {
  inTransaction: boolean;
}

export interface DatabaseInfo extends Pending {
  path: string;
  version: string;
  readOnly: boolean;
  /** Function names for the SQL editor to offer. */
  functions: string[];
}

export interface ColumnInfo {
  name: string;
  type: string;
  affinity: string;
  primaryKey: boolean;
  notNull: boolean;
  unique: boolean;
  generated: number;
  defaultSql: string | null;
}

export interface TableInfo {
  name: string;
  sql: string;
  strict: boolean;
  withoutRowid: boolean;
  columns: ColumnInfo[];
}

export interface ViewInfo {
  name: string;
  sql: string;
  columns: ColumnInfo[];
}

export interface IndexInfo {
  name: string;
  table: string;
  unique: boolean;
  partial: boolean;
  sql: string;
  columns: string[];
}

export interface TriggerInfo {
  name: string;
  table: string;
  sql: string;
}

export interface Schema {
  tables: TableInfo[];
  views: ViewInfo[];
  indexes: IndexInfo[];
  triggers: TriggerInfo[];
}

export interface SchemaReply extends Schema, Pending {}

export interface SortSpec {
  column: string;
  descending: boolean;
}

export interface BrowseRequest {
  table: string;
  offset: number;
  limit: number;
  sort: SortSpec[];
  filters: Record<string, string>;
}

export interface BrowseReply extends Pending {
  table: string;
  view: boolean;
  editable: boolean;
  columns: ColumnInfo[];
  rows: Cell[][];
  /** What identifies each row for update and delete; one entry per row. */
  keys: Cell[][];
  total: number;
  offset: number;
  limit: number;
}

export interface SqlReply extends Pending {
  columns: string[];
  rows: Cell[][];
  rowCount: number;
  truncated: boolean;
  changes: number;
  lastInsertId: number;
  elapsedMs: number;
}

export interface ChangeReply extends Pending {
  changes: number;
  lastInsertId?: number;
}

export interface PragmaChoice {
  value: number;
  label: string;
}

/** Where a pragma lives: in the database file, in the connection of the tool, or only reported by the store. */
export type PragmaScope = 'file' | 'session' | 'store';

export interface PragmaInfo {
  name: string;
  kind: 'boolean' | 'integer' | 'choice' | 'text';
  scope: PragmaScope;
  description: string;
  readOnly: boolean;
  value: Cell;
  choices: PragmaChoice[];
}

export interface PragmasReply extends Pending {
  pragmas: PragmaInfo[];
}

export interface DumpReply extends Pending {
  sql: string;
}
