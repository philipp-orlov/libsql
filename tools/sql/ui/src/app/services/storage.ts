// What the Visualize tab reads: the store underneath the database, through PRAGMA storage_* (libsql
// src/internal/pragma_storage.cpp) over the same SQL socket as every other tab. Each function takes the connection
// it asks through and returns plain data. Everything is the committed state of the store, not a pending transaction.

import { Cell } from '../models/api.models';
import { Conn, int, rowsOf, text } from './catalog';

export type PageKind = 'meta' | 'branch' | 'leaf' | 'overflow' | 'overflow-continued' | 'free' | 'unknown';
export type TreeRole = 'free' | 'main' | 'catalog' | 'named';
/** What a tree holds, for the list: a SQL table or index, or part of the store itself. */
export type TreeKind = 'table' | 'index' | 'catalog' | 'free' | 'main' | 'other';

export interface StoreStats {
  [name: string]: number;
}

export interface StorageTree {
  id: number;
  /** The sub-database's name ("t#1"), empty for the store's own trees. */
  name: string;
  role: TreeRole;
  kind: TreeKind;
  /** The SQL table or index it holds. */
  object: string | null;
  /** The table of an index. */
  parent: string | null;
  /** null when the tree is empty. */
  root: number | null;
  depth: number;
  branchPages: number;
  leafPages: number;
  overflowPages: number;
  entries: number;
  flags: number;
}

export interface MapRun {
  first: number;
  count: number;
  kind: PageKind;
  /** Index into the trees; null for meta, free and unknown pages. */
  tree: number | null;
}

export interface StorageMeta {
  slot: number;
  valid: boolean;
  current: boolean;
  version: number;
  pageSize: number;
  txnid: number;
  lastPgno: number;
  fileSize: number;
  namedDbs: number;
  checksum: string;
  computedChecksum: string;
  storeId: string;
  commitId: string;
  parentId: string;
  parentChecksum: string;
  deferredCount: number;
  inlineCount: number;
  inlineTxn: number;
}

export interface FreeEntry {
  txn: number;
  chunk: number;
  pages: number;
  firstPages: number[];
  deferred: boolean;
}

export interface PageDetails {
  pgno: number;
  kind: PageKind;
  flags: number;
  prefixKeys: boolean;
  suffixKeys: boolean;
  pageSize: number;
  nkeys: number;
  lower: number;
  upper: number;
  sharedBytes: number;
  sharedBlock: number;
  freeBytes: number;
  usedBytes: number;
  /** Leaf entries in the page and below it. */
  entries: number;
  runPages: number;
  runBytes: number;
  storedHash: string;
  computedHash: string;
  hashOk: boolean;
  headerOk: boolean;
}

export interface PageNode {
  index: number;
  offset: number;
  size: number;
  key: Uint8Array;
  keyBytes: number;
  valueBytes: number;
  /** The first bytes of the value (up to 256). */
  value: Uint8Array;
  big: boolean;
  subdb: boolean;
  child: number;
  count: number;
  overflow: number;
  /** The key as a rowid, when the tree is a rowid table. */
  rowid: number | null;
  /** The key as libsql's ordered key encoding, e.g. an index entry or the key of a WITHOUT ROWID table: `('abc', 7)`. */
  keyText: string | null;
  /** The value as a stored row: `(NULL, 'abc', 7)`. */
  rowText: string | null;
}

/** What the explorer shows of a child page without opening it. */
export interface ChildSummary {
  pgno: number;
  kind: PageKind;
  nkeys: number;
  freeBytes: number;
  usedBytes: number;
  pageSize: number;
  entries: number;
  hashOk: boolean;
  headerOk: boolean;
}

export interface CheckFinding {
  result: 'page' | 'integrity' | 'ok';
  page: number | null;
  detail: string;
}

export interface CheckReport {
  ok: boolean;
  pagesChecked: number;
  badPages: number[];
  integrity: string;
}

const KINDS: readonly PageKind[] = ['meta', 'branch', 'leaf', 'overflow', 'overflow-continued', 'free', 'unknown'];

function kindOf(cell: Cell | undefined): PageKind {
  const name = text(cell);
  return (KINDS as readonly string[]).includes(name) ? (name as PageKind) : 'unknown';
}

const bool = (cell: Cell | undefined): boolean => int(cell) !== 0;

/** A page number goes into the SQL as a number, never as text. */
function pageNumber(pgno: number): number {
  if (!Number.isSafeInteger(pgno) || pgno < 0) {
    throw new Error('bad page number');
  }
  return pgno;
}

// --------------------------------------------------------------------------------------------------- the store ---

export async function loadStats(conn: Conn): Promise<StoreStats> {
  const stats: StoreStats = {};
  for (const [name, value] of rowsOf(await conn.exec('SELECT name, value FROM pragma_storage_stats()'))) {
    stats[text(name)] = int(value);
  }
  return stats;
}

export async function loadMeta(conn: Conn): Promise<StorageMeta[]> {
  const rows = rowsOf(await conn.exec('SELECT * FROM pragma_storage_meta() ORDER BY slot'));
  return rows.map(r => ({
    slot: int(r[0]),
    valid: bool(r[1]),
    current: bool(r[2]),
    version: int(r[3]),
    pageSize: int(r[4]),
    txnid: int(r[5]),
    lastPgno: int(r[6]),
    fileSize: int(r[7]),
    namedDbs: int(r[8]),
    checksum: text(r[9]),
    computedChecksum: text(r[10]),
    storeId: text(r[11]),
    commitId: text(r[12]),
    parentId: text(r[13]),
    parentChecksum: text(r[14]),
    deferredCount: int(r[15]),
    inlineCount: int(r[16]),
    inlineTxn: int(r[17]),
  }));
}

export async function loadTrees(conn: Conn): Promise<StorageTree[]> {
  const rows = rowsOf(await conn.exec('SELECT * FROM pragma_storage_trees() ORDER BY id'));
  return rows.map(r => ({
    id: int(r[0]),
    name: text(r[1]),
    role: text(r[2]) as TreeRole,
    kind: text(r[3]) as TreeKind,
    object: r[4] === null ? null : text(r[4]),
    parent: r[5] === null ? null : text(r[5]),
    root: r[6] === null ? null : int(r[6]),
    depth: int(r[7]),
    branchPages: int(r[8]),
    leafPages: int(r[9]),
    overflowPages: int(r[10]),
    entries: int(r[11]),
    flags: int(r[12]),
  }));
}

export async function loadMap(conn: Conn): Promise<MapRun[]> {
  const rows = rowsOf(await conn.exec('SELECT first, count, kind, tree FROM pragma_storage_map()'));
  return rows.map(r => ({ first: int(r[0]), count: int(r[1]), kind: kindOf(r[2]), tree: r[3] === null ? null : int(r[3]) }));
}

export async function loadFree(conn: Conn): Promise<FreeEntry[]> {
  const rows = rowsOf(await conn.exec('SELECT txn, chunk, pages, first_pages, deferred FROM pragma_storage_free()'));
  return rows.map(r => ({
    txn: int(r[0]),
    chunk: int(r[1]),
    pages: int(r[2]),
    firstPages: text(r[3])
      .split(',')
      .filter(Boolean)
      .map(Number),
    deferred: bool(r[4]),
  }));
}

export async function runCheck(conn: Conn): Promise<CheckReport> {
  const rows = rowsOf(await conn.exec('SELECT result, page, detail, pages_checked FROM pragma_storage_check()'));
  const report: CheckReport = { ok: false, pagesChecked: 0, badPages: [], integrity: '' };
  for (const [result, page, detail, checked] of rows) {
    report.pagesChecked = int(checked);
    if (text(result) === 'ok') {
      report.ok = true;
    } else if (text(result) === 'page' && page !== null) {
      report.badPages.push(int(page));
    } else if (text(result) === 'integrity') {
      report.integrity = text(detail);
    }
  }
  return report;
}

// --------------------------------------------------------------------------------------------------- one page ---

const PAGE_COLUMNS =
  'pgno, kind, flags, prefix_keys, suffix_keys, page_size, nkeys, lower, upper, shared_bytes, shared_block, free_bytes, used_bytes, entries, run_pages, run_bytes, stored_hash, computed_hash, hash_ok, header_ok';

function pageOf(r: Cell[]): PageDetails {
  return {
    pgno: int(r[0]),
    kind: kindOf(r[1]),
    flags: int(r[2]),
    prefixKeys: bool(r[3]),
    suffixKeys: bool(r[4]),
    pageSize: int(r[5]),
    nkeys: int(r[6]),
    lower: int(r[7]),
    upper: int(r[8]),
    sharedBytes: int(r[9]),
    sharedBlock: int(r[10]),
    freeBytes: int(r[11]),
    usedBytes: int(r[12]),
    entries: int(r[13]),
    runPages: int(r[14]),
    runBytes: int(r[15]),
    storedHash: text(r[16]),
    computedHash: text(r[17]),
    hashOk: bool(r[18]),
    headerOk: bool(r[19]),
  };
}

export async function loadPage(conn: Conn, pgno: number): Promise<PageDetails> {
  const rows = rowsOf(await conn.exec(`SELECT ${PAGE_COLUMNS} FROM pragma_storage_page(${pageNumber(pgno)})`));
  if (rows.length === 0) {
    throw new Error(`page ${pgno} cannot be read`);
  }
  return pageOf(rows[0]);
}

/** Every node of a branch or leaf page. `kind` is the tree's, which says how its keys and rows are read. */
export async function loadNodes(conn: Conn, pgno: number, kind: TreeKind | null): Promise<PageNode[]> {
  const rowid = kind === 'table' ? 'storage_rowid(key)' : 'NULL';
  // an index key is its columns then the rowid; a WITHOUT ROWID table's key is columns only; a rowid table's is a bare rowid
  const key = kind === 'index' ? 'storage_key(key, 1)' : kind === 'table' ? 'storage_key(key)' : 'NULL';
  const row = kind === 'table' || kind === 'index' ? 'storage_row(value)' : 'NULL';
  const result = await conn.exec(
    `SELECT idx, offset, size, key, key_bytes, value_bytes, value, big, subdb, child, count, overflow, ${rowid}, ${key}, ${row} FROM pragma_storage_nodes(${pageNumber(pgno)}) ORDER BY idx`,
  );
  const view = result.view;
  if (!view) {
    return [];
  }
  const nodes: PageNode[] = [];
  for (let r = 0; r < view.rowCount; r++) {
    const c = (column: number) => view.cell(r, column);
    nodes.push({
      index: int(c(0)),
      offset: int(c(1)),
      size: int(c(2)),
      key: view.raw(r, 3),
      keyBytes: int(c(4)),
      valueBytes: int(c(5)),
      value: view.raw(r, 6),
      big: bool(c(7)),
      subdb: bool(c(8)),
      child: int(c(9)),
      count: int(c(10)),
      overflow: int(c(11)),
      rowid: c(12) === null ? null : int(c(12)),
      keyText: c(13) === null ? null : text(c(13)),
      rowText: c(14) === null ? null : text(c(14)),
    });
  }
  return nodes;
}

/** The raw bytes of a page (an overflow run: its first pages, see `pages`). */
export async function loadBytes(conn: Conn, pgno: number): Promise<{ bytes: Uint8Array; pages: number }> {
  const result = await conn.exec(`SELECT data, pages FROM pragma_storage_bytes(${pageNumber(pgno)})`);
  if (!result.view || result.view.rowCount === 0) {
    throw new Error(`page ${pgno} cannot be read`);
  }
  return { bytes: result.view.raw(0, 0), pages: int(result.view.cell(0, 1)) };
}

/** The children of a branch page with their headers, in one request. */
export async function loadChildren(conn: Conn, pgno: number): Promise<ChildSummary[]> {
  const rows = rowsOf(
    await conn.exec(
      `SELECT n.child, p.kind, p.nkeys, p.free_bytes, p.used_bytes, p.page_size, p.entries, p.hash_ok, p.header_ok ` +
        `FROM pragma_storage_nodes(${pageNumber(pgno)}) n, pragma_storage_page(n.child) p WHERE n.child >= 2 ORDER BY n.idx`,
    ),
  );
  return rows.map(r => ({
    pgno: int(r[0]),
    kind: kindOf(r[1]),
    nkeys: int(r[2]),
    freeBytes: int(r[3]),
    usedBytes: int(r[4]),
    pageSize: int(r[5]),
    entries: int(r[6]),
    hashOk: bool(r[7]),
    headerOk: bool(r[8]),
  }));
}

// ----------------------------------------------------------------------------------------------- presentation ---

/** Flags of a page header, named. */
export function flagNames(flags: number): string[] {
  const names: [number, string][] = [
    [1, 'BRANCH'],
    [2, 'LEAF'],
    [4, 'OVERFLOW'],
    [8, 'META'],
    [16, 'PREFIX'],
    [32, 'SUFFIX'],
  ];
  return names.filter(([bit]) => (flags & bit) !== 0).map(([, name]) => name);
}

/** The bytes as a string when they are printable UTF-8, else null. */
export function printable(bytes: Uint8Array): string | null {
  if (bytes.length === 0) {
    return '';
  }
  let decoded: string;
  try {
    decoded = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  } catch {
    return null;
  }
  // eslint-disable-next-line no-control-regex
  return /[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(decoded) ? null : decoded;
}

export function toHex(bytes: Uint8Array, limit = bytes.length): string {
  let out = '';
  const n = Math.min(bytes.length, limit);
  for (let i = 0; i < n; i++) {
    out += (i ? ' ' : '') + bytes[i].toString(16).padStart(2, '0');
  }
  return out + (bytes.length > n ? ' …' : '');
}

/** A key or value for a table cell: text when it is text, else hex. */
export function preview(bytes: Uint8Array, total: number, limit = 48): string {
  const asText = printable(bytes.subarray(0, limit));
  const shown = asText !== null ? asText : toHex(bytes, Math.min(limit / 2, 16));
  return total > Math.min(bytes.length, limit) ? shown + ' …' : shown;
}

export function formatBytes(n: number): string {
  if (n < 1024) {
    return `${n} B`;
  }
  const units = ['KiB', 'MiB', 'GiB', 'TiB'];
  let value = n / 1024;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  return `${value >= 100 ? value.toFixed(0) : value.toFixed(1)} ${units[unit]}`;
}

/** What a tree is called in the list. */
export function treeLabel(tree: StorageTree): string {
  switch (tree.kind) {
    case 'table':
      return tree.object ?? tree.name;
    case 'index':
      return `${tree.object ?? tree.name}`;
    case 'catalog':
      return tree.name === 'sql_catalog' ? 'SQL catalog' : 'Sub-database catalog';
    case 'free':
      return 'Free list';
    case 'main':
      return 'Main tree';
    default:
      return tree.name || 'tree';
  }
}

/** Pages that belong to a tree, as the map counts them. */
export function treePages(tree: StorageTree): number {
  return tree.branchPages + tree.leafPages + tree.overflowPages;
}
