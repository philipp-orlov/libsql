import { Cell, ColumnInfo } from '../models/api.models';

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
