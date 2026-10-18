import { BlobCell, Cell, ColumnInfo, IntegerCell } from '../models/api.models';
import type { GridColumn } from '../shared/data-grid/data-grid.models';

export type CellKind = 'null' | 'integer' | 'real' | 'text' | 'blob';

export function cellKind(cell: Cell): CellKind {
  if (cell === null) {
    return 'null';
  }
  if (typeof cell === 'number') {
    return Number.isInteger(cell) ? 'integer' : 'real';
  }
  if (typeof cell === 'string') {
    return 'text';
  }
  return cell.type === 'blob' ? 'blob' : 'integer';
}

/** What the grid shows. */
export function cellText(cell: Cell): string {
  if (cell === null) {
    return 'NULL';
  }
  if (typeof cell === 'object') {
    return cell.type === 'blob' ? `BLOB (${cell.size} bytes)` : cell.value;
  }
  return String(cell);
}

/** What the cell editor starts with. */
export function cellEditText(cell: Cell): string {
  return cell === null || cellKind(cell) === 'blob' ? '' : cellText(cell);
}

/** The grid cannot send a blob back, so it does not offer to edit one. */
export function isEditableCell(cell: Cell): boolean {
  return cellKind(cell) !== 'blob';
}

const KIND_RANK: Record<CellKind, number> = { null: 0, integer: 1, real: 1, text: 2, blob: 3 };

function compareNumbers(a: number | IntegerCell, b: number | IntegerCell): number {
  if (typeof a === 'number' && typeof b === 'number') {
    return Math.sign(a - b);
  }
  // An integer beyond double precision: exact against an integer, close enough against a real.
  const exact = (n: number | IntegerCell) => (typeof n === 'number' ? (Number.isInteger(n) ? BigInt(n) : null) : BigInt(n.value));
  const x = exact(a);
  const y = exact(b);
  if (x !== null && y !== null) {
    return x < y ? -1 : x > y ? 1 : 0;
  }
  return Math.sign(Number(typeof a === 'number' ? a : a.value) - Number(typeof b === 'number' ? b : b.value));
}

/** SQLite's order of values: NULL, numbers, text, then blobs. */
export function compareCells(a: unknown, b: unknown): number {
  const x = a as Cell;
  const y = b as Cell;
  const kx = cellKind(x);
  const ky = cellKind(y);
  if (KIND_RANK[kx] !== KIND_RANK[ky]) {
    return KIND_RANK[kx] - KIND_RANK[ky];
  }
  switch (KIND_RANK[kx]) {
    case 0:
      return 0;
    case 1:
      return compareNumbers(x as number | IntegerCell, y as number | IntegerCell);
    case 2:
      return (x as string) < (y as string) ? -1 : (x as string) > (y as string) ? 1 : 0;
    default: {
      const p = (x as BlobCell).value;
      const q = (y as BlobCell).value;
      return p < q ? -1 : p > q ? 1 : (x as BlobCell).size - (y as BlobCell).size;
    }
  }
}

/** A grid column over rows of cells: cell `index`, shown, sorted and grouped the way SQLite sees the values. */
export function cellColumn(field: string, title: string, index: number): GridColumn<Cell[]> {
  return { field, title, value: row => row[index], text: value => cellText(value as Cell), compare: compareCells };
}

const SAFE_INTEGER = /^[+-]?\d{1,15}$/;
const INTEGER = /^[+-]?\d+$/;
const REAL = /^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/;

/**
 * What the user typed in a cell, as the value to send. Text typed into a numeric column goes as a number,
 * so the answer does not depend on the column's affinity rules.
 */
export function inputToCell(text: string, column: Pick<ColumnInfo, 'affinity'>): Cell {
  const numeric = column.affinity === 'INTEGER' || column.affinity === 'REAL' || column.affinity === 'NUMERIC';
  const trimmed = text.trim();
  if (numeric && SAFE_INTEGER.test(trimmed)) {
    return Number(trimmed);
  }
  if (numeric && INTEGER.test(trimmed)) {
    return { type: 'integer', value: trimmed.replace(/^\+/, '') };
  }
  if (numeric && column.affinity !== 'INTEGER' && REAL.test(trimmed)) {
    return Number(trimmed);
  }
  return text;
}

function csvField(text: string): string {
  return /[",\r\n]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
}

/** RFC 4180 text; NULL is an empty field. */
export function toCsv(columns: string[], rows: Cell[][]): string {
  const lines = [columns.map(csvField).join(',')];
  for (const row of rows) {
    lines.push(row.map(cell => (cell === null ? '' : csvField(cellText(cell)))).join(','));
  }
  return lines.join('\r\n') + '\r\n';
}
