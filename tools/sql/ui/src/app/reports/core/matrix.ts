// Copyright (c) 2026 Philipp Orlov

// A cross-tab: the rows of a dataset pivoted so that one field's values run across the top, one or two fields' values run
// down the side, and each cell aggregates a measure. Also the color scales ("highlight table") that tint the cells.
// Free of Angular imports on purpose.

import { Aggregate, HeatScale, MatrixItem } from './definition';
import { formatValue, Value } from './format';
import { columnIndex, compareValues, DataSet } from './scope';

export interface MatrixColumn {
  key: Value;
  label: string;
}

export interface MatrixRow {
  /** `group`: the outer heading of two row fields; `data`: a row; `total`: the bottom line. */
  kind: 'group' | 'data' | 'total';
  /** 0 for outer headings, the grand total and rows of a one-level matrix; 1 for rows inside a group. */
  level: number;
  label: string;
  /** The values of the row fields that lead to this row (for the drill-through and the highlight). */
  keys: Value[];
  /** One per column; null where nothing falls in. */
  values: (number | null)[];
  total: number | null;
}

export interface MatrixModel {
  corner: string;
  columns: MatrixColumn[];
  rows: MatrixRow[];
  /** The range of the body cells (what the color scale spans). */
  min: number;
  max: number;
  /** The columns left out because they would not fit. */
  cut: number;
  warnings: string[];
}

class Acc {
  sum = 0;
  n = 0;
  min = Infinity;
  max = -Infinity;
  rows = 0;
  add(v: Value): void {
    this.rows++;
    const x = typeof v === 'number' ? v : typeof v === 'string' && v.trim() !== '' && Number.isFinite(Number(v)) ? Number(v) : null;
    if (x === null) return;
    this.n++;
    this.sum += x;
    if (x < this.min) this.min = x;
    if (x > this.max) this.max = x;
  }
  result(agg: Aggregate): number | null {
    switch (agg) {
      case 'count': return this.n;
      case 'avg': return this.n ? this.sum / this.n : null;
      case 'min': return this.n ? this.min : null;
      case 'max': return this.n ? this.max : null;
      default: return this.n ? this.sum : null;
    }
  }
}

const SEP = '\u0001';

/** Pivots `ds` as `it` describes. `maxColumns` is how many columns the page has room for. */
export function matrixModel(it: MatrixItem, ds: DataSet, label: string, maxColumns: number, sort?: { column: number; dir: 'asc' | 'desc' }): MatrixModel {
  const warnings: string[] = [];
  const rowIdx = it.rows.map(f => columnIndex(ds, f));
  const colIdx = columnIndex(ds, it.column);
  const valIdx = columnIndex(ds, it.value);
  it.rows.forEach((f, i) => rowIdx[i] < 0 && warnings.push(`${label}: dataset ${it.dataset} has no column ${f}`));
  if (colIdx < 0) warnings.push(`${label}: dataset ${it.dataset} has no column ${it.column}`);
  if (valIdx < 0) warnings.push(`${label}: dataset ${it.dataset} has no column ${it.value}`);
  const corner = it.corner ?? it.rows.join(' / ');
  if (warnings.length) return { corner, columns: [], rows: [], min: 0, max: 0, cut: 0, warnings };

  const agg = it.agg ?? 'sum';
  const cells = new Map<string, Acc>();
  const rowAcc = new Map<string, Acc>();
  const colAcc = new Map<string, Acc>();
  const groupAcc = new Map<string, Acc>();
  const groupCol = new Map<string, Acc>();
  const grand = new Acc();
  const colKeys = new Map<string, Value>();
  const rowKeys = new Map<string, Value[]>();
  const get = (m: Map<string, Acc>, k: string): Acc => {
    let a = m.get(k);
    if (!a) m.set(k, (a = new Acc()));
    return a;
  };
  for (const r of ds.rows) {
    const ck = String(r[colIdx]);
    const keys = rowIdx.map(i => r[i]);
    const rk = keys.map(String).join(SEP);
    const v = r[valIdx];
    if (!colKeys.has(ck)) colKeys.set(ck, r[colIdx]);
    if (!rowKeys.has(rk)) rowKeys.set(rk, keys);
    get(cells, rk + SEP + SEP + ck).add(v);
    get(rowAcc, rk).add(v);
    get(colAcc, ck).add(v);
    if (keys.length > 1) {
      get(groupAcc, String(keys[0])).add(v);
      get(groupCol, String(keys[0]) + SEP + ck).add(v);
    }
    grand.add(v);
  }

  // the order of the headings
  const order = <T>(items: T[], key: (t: T) => Value, how: MatrixItem['sortColumns'] | MatrixItem['sortRows']): T[] => {
    if (how === 'asc') return [...items].sort((a, b) => compareValues(key(a), key(b)));
    if (how === 'desc') return [...items].sort((a, b) => compareValues(key(b), key(a)));
    return items;
  };
  let columns: MatrixColumn[] = order([...colKeys.entries()], e => e[1], it.sortColumns).map(([, key]) => ({ key, label: formatValue(key) }));
  let cut = 0;
  if (columns.length > maxColumns) {
    cut = columns.length - maxColumns;
    columns = columns.slice(0, maxColumns);
    warnings.push(`${label}: only the first ${maxColumns} of ${maxColumns + cut} columns fit the page`);
  }
  const colNames = columns.map(c => String(c.key));

  const valuesOf = (prefix: string, lookup: Map<string, Acc>, groupLevel = false): (number | null)[] =>
    colNames.map(ck => lookup.get(prefix + (groupLevel ? SEP : SEP + SEP) + ck)?.result(agg) ?? null);

  const two = it.rows.length === 2;
  const dataRow = (keys: Value[]): MatrixRow => {
    const rk = keys.map(String).join(SEP);
    return { kind: 'data', level: two ? 1 : 0, label: formatValue(keys[keys.length - 1]), keys, values: valuesOf(rk, cells), total: rowAcc.get(rk)?.result(agg) ?? null };
  };
  const rowTotal = (r: MatrixRow): number | null => r.total;
  const sortKey = (r: MatrixRow): number | null => (sort ? (sort.column >= columns.length ? r.total : r.values[sort.column]) : null);
  const bySort = (a: MatrixRow, b: MatrixRow): number => {
    const x = sortKey(a);
    const y = sortKey(b);
    if (x === null || y === null) return x === y ? 0 : x === null ? 1 : -1;
    return sort!.dir === 'desc' ? y - x : x - y;
  };
  const arrange = (rows: MatrixRow[]): MatrixRow[] => {
    if (sort) return [...rows].sort(bySort);
    if (it.sortRows === 'total') return [...rows].sort((a, b) => (rowTotal(b) ?? -Infinity) - (rowTotal(a) ?? -Infinity));
    return order(rows, r => r.keys[r.keys.length - 1], it.sortRows);
  };

  const rows: MatrixRow[] = [];
  if (!two) {
    rows.push(...arrange([...rowKeys.values()].map(dataRow)));
  } else {
    const groups = new Map<string, Value[][]>();
    for (const keys of rowKeys.values()) {
      const g = String(keys[0]);
      if (!groups.has(g)) groups.set(g, []);
      groups.get(g)!.push(keys);
    }
    let heads = [...groups.entries()].map(([g, list]) => ({
      g,
      key: list[0][0],
      row: { kind: 'group', level: 0, label: formatValue(list[0][0]), keys: [list[0][0]], values: it.subtotals ? valuesOf(g, groupCol, true) : colNames.map(() => null), total: it.subtotals ? groupAcc.get(g)?.result(agg) ?? null : null } as MatrixRow,
      list,
    }));
    if (sort && it.subtotals) heads = [...heads].sort((a, b) => bySort(a.row, b.row));
    else if (it.sortRows === 'total') heads = [...heads].sort((a, b) => (groupAcc.get(b.g)?.result(agg) ?? -Infinity) - (groupAcc.get(a.g)?.result(agg) ?? -Infinity));
    else heads = order(heads, h => h.key, it.sortRows);
    for (const h of heads) {
      rows.push(h.row, ...arrange(h.list.map(dataRow)));
    }
  }

  let min = Infinity;
  let max = -Infinity;
  for (const r of rows) {
    if (r.kind !== 'data') continue;
    for (const v of r.values) {
      if (v === null) continue;
      if (v < min) min = v;
      if (v > max) max = v;
    }
  }
  if (!Number.isFinite(min)) min = max = 0;

  if (it.columnTotals) rows.push({ kind: 'total', level: 0, label: 'Total', keys: [], values: colNames.map(ck => colAcc.get(ck)?.result(agg) ?? null), total: grand.result(agg) });
  return { corner, columns, rows, min, max, cut, warnings };
}

// ---- color scales -----------------------------------------------------------------------------------------------------

const HUES: Record<Exclude<HeatScale, 'none' | 'diverging'>, string> = {
  blue: '#2a6fb0',
  green: '#3b8f4d',
  orange: '#e0781c',
  red: '#cc3b3b',
  purple: '#7a4fa0',
  teal: '#1a8a8a',
};
const NEGATIVE = '#d6604d';
const POSITIVE = '#3a7ebf';

const rgb = (c: string): [number, number, number] => {
  const h = c.replace('#', '');
  const n = parseInt(h.length === 3 ? [...h].map(x => x + x).join('') : h, 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
};
const hex = (c: [number, number, number]): string => '#' + c.map(v => Math.round(v).toString(16).padStart(2, '0')).join('');
/** `t` of the way from white to `to`. */
const tint = (to: string, t: number): string => hex(rgb(to).map(v => 255 + (v - 255) * t) as [number, number, number]);

/** The fill of a cell valued `v` among cells from `min` to `max`, or undefined for none. */
export function heatFill(scale: HeatScale | undefined, v: number, min: number, max: number): string | undefined {
  const s = scale ?? 'blue';
  if (s === 'none') return undefined;
  if (s === 'diverging') {
    const mid = min < 0 && max > 0 ? 0 : (min + max) / 2;
    if (v >= mid) return max > mid ? tint(POSITIVE, 0.12 + 0.88 * ((v - mid) / (max - mid))) : '#ffffff';
    return mid > min ? tint(NEGATIVE, 0.12 + 0.88 * ((mid - v) / (mid - min))) : '#ffffff';
  }
  const t = max > min ? (v - min) / (max - min) : 0.5;
  return tint(HUES[s] ?? HUES.blue, 0.08 + 0.92 * t);
}

/** The text color that reads on `fill`. */
export function inkOn(fill: string | undefined): string {
  if (!fill) return '#222222';
  const [r, g, b] = rgb(fill);
  return 0.299 * r + 0.587 * g + 0.114 * b < 140 ? '#ffffff' : '#222222';
}

/** Five swatches for a legend: [value, fill]. */
export function heatRamp(scale: HeatScale | undefined, min: number, max: number): [number, string][] {
  const out: [number, string][] = [];
  for (let i = 0; i < 5; i++) {
    const v = min + ((max - min) * i) / 4;
    out.push([v, heatFill(scale, v, min, max) ?? '#ffffff']);
  }
  return out;
}
