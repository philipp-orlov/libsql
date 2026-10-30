// Copyright (c) 2026 Philipp Orlov

// What a table item shows, as rows: group headings, data rows, subtotals and the grand total, with the text and alignment
// of every cell. The layout draws these rows; the CSV, Excel and Word exports write them. Free of Angular imports on purpose.

import { ColumnDef, TableItem } from './definition';
import { evaluate, interpolate } from './expr';
import { formatValue, Value } from './format';
import { columnIndex, compareValues, context, DataSet, Env } from './scope';

export type Align = 'left' | 'center' | 'right';

export interface ColumnInfo {
  def: ColumnDef;
  idx: number;
  numeric: boolean;
  /** The heading's alignment. */
  align: Align;
  header: string;
}

export interface CellText {
  text: string;
  align: Align;
  /** The number or text behind `text`, for the exports (totals have one too). */
  value?: Value;
}

export interface TableRow {
  kind: 'group' | 'data' | 'total' | 'grand';
  /** How deep in the groups (the layout indents by it). */
  level: number;
  cells: CellText[];
  /** Data rows: the column values, unformatted. */
  values?: Value[];
  /** Data rows: the row of the dataset. */
  source?: Value[];
  /** Group rows: the fold id, whether it is folded, and how many rows it holds. */
  id?: string;
  folded?: boolean;
  count?: number;
  /** Group rows: start a new page before this one. */
  pageBreak?: boolean;
}

export interface TableModel {
  columns: ColumnInfo[];
  rows: TableRow[];
  /** The column that carries the word "Total". */
  labelAt: number;
  sorting?: { column: number; dir: 'asc' | 'desc' };
  warnings: string[];
}

export interface TableView {
  sort?: { column: number; dir: 'asc' | 'desc' };
  folded?: string[];
}

/** `id` names the item for the fold ids; `label` is how warnings call it. Folds are ignored when `view.folded` is undefined. */
export function tableModel(it: TableItem, ds: DataSet, env: Env, view: TableView | undefined, id: string, label: string): TableModel {
  const warnings: string[] = [];
  const cols: ColumnInfo[] = it.columns.map(def => ({ def, idx: def.field ? columnIndex(ds, def.field) : -1, numeric: false, align: 'left', header: def.header ?? def.field ?? '' }));
  for (const c of cols) if (c.def.field && c.idx < 0) warnings.push(`${label}: dataset ${it.dataset} has no column ${c.def.field}`);
  // A column of numbers is right-aligned, heading included, unless the definition says otherwise.
  for (const c of cols) {
    if (c.idx >= 0) c.numeric = typeof (ds.rows.find(r => r[c.idx] !== null)?.[c.idx] ?? null) === 'number';
    else if (c.def.expression && ds.rows.length) {
      try {
        c.numeric = typeof evaluate(c.def.expression, context(env, ds, ds.rows, ds.rows[0])) === 'number';
      } catch {
        c.numeric = false;
      }
    }
    c.align = c.def.align ?? (c.numeric ? 'right' : 'left');
  }
  const groups = (it.groups ?? []).map(g => ({ g, idx: columnIndex(ds, g.field) }));
  for (const g of groups) if (g.idx < 0) warnings.push(`${label}: no column ${g.g.field} to group by`);
  const used = groups.filter(g => g.idx >= 0);

  const cell = (c: ColumnInfo, row: Value[], scope: Value[][]): Value => {
    if (c.def.expression) {
      try {
        return evaluate(c.def.expression, context(env, ds, scope, row));
      } catch {
        return '#Error';
      }
    }
    return c.idx >= 0 ? row[c.idx] ?? null : null;
  };

  // The rows in the order shown: by the group fields, then (when the reader clicked a heading) by that column.
  const sorting = view?.sort;
  const sortCol = sorting ? cols[sorting.column] : undefined;
  let rows = ds.rows;
  if (used.length || sortCol) {
    const keyed = rows.map(r => ({ r, k: sortCol ? cell(sortCol, r, ds.rows) : null }));
    keyed.sort((a, b) => {
      for (const g of used) {
        const c = compareValues(a.r[g.idx], b.r[g.idx]);
        if (c) return g.g.sort === 'desc' ? -c : c;
      }
      return sortCol ? compareValues(a.k, b.k) * (sorting!.dir === 'desc' ? -1 : 1) : 0;
    });
    rows = keyed.map(x => x.r);
  }

  // The label of a total goes in the first column that holds text and has no total of its own (a narrow rank or id
  // column in front of it would wrap the word).
  let labelAt = cols.findIndex(c => !c.def.total && !c.numeric);
  if (labelAt < 0) labelAt = cols.findIndex(c => !c.def.total);
  const totalTexts = (scope: Value[][]): CellText[] =>
    cols.map(c => {
      if (!c.def.total) return { text: '', align: 'left' as const };
      const vals = scope.map(r => cell(c, r, scope));
      const nums = vals.filter((v): v is number => typeof v === 'number');
      let v: Value = null;
      switch (c.def.total) {
        case 'sum': v = nums.reduce((a, b) => a + b, 0); break;
        case 'avg': v = nums.length ? nums.reduce((a, b) => a + b, 0) / nums.length : null; break;
        case 'min': v = nums.length ? Math.min(...nums) : null; break;
        case 'max': v = nums.length ? Math.max(...nums) : null; break;
        case 'count': v = vals.filter(x => x !== null).length; break;
      }
      return { text: formatValue(v, c.def.format ?? ''), align: c.def.align ?? 'right', value: v };
    });
  const totalRow = (kind: 'total' | 'grand', label: string, scope: Value[][], level: number): TableRow => ({
    kind,
    level,
    cells: totalTexts(scope).map((t, i) => (i === labelAt ? { text: label, align: 'left' as const, value: label } : t)),
  });

  const out: TableRow[] = [];
  const folded = new Set(view?.folded ?? []);
  const emit = (scope: Value[][], level: number, path: string): void => {
    if (level < used.length) {
      const g = used[level];
      let first = true;
      for (let i = 0; i < scope.length; ) {
        let j = i + 1;
        while (j < scope.length && scope[j][g.idx] === scope[i][g.idx]) j++;
        const run = scope.slice(i, j);
        const cx = context(env, ds, run, run[0]);
        const heading = interpolate(g.g.header ?? `{{[${g.g.field}]}}`, cx);
        const gid = `${id}:${level}:${path}${String(run[0][g.idx])}`;
        const isFolded = folded.has(gid);
        // a folded group shows its totals in its heading, so folding keeps the numbers
        const summary = isFolded && !g.g.subtotals ? totalTexts(run) : cols.map(() => ({ text: '', align: 'left' as const }));
        out.push({ kind: 'group', level, cells: [{ text: heading, align: 'left', value: heading }, ...summary.slice(1)], id: gid, folded: isFolded, count: run.length, pageBreak: !!g.g.pageBreak && !first });
        first = false;
        if (!isFolded) emit(run, level + 1, `${path}${String(run[0][g.idx])}\u0001`);
        if (g.g.subtotals) out.push(totalRow('total', `Total ${formatValue(run[0][g.idx] ?? null)}`, run, level));
        i = j;
      }
      return;
    }
    for (const row of scope) {
      const values = cols.map(c => cell(c, row, scope));
      out.push({
        kind: 'data',
        level,
        values,
        source: row,
        cells: cols.map((c, k) => ({ text: formatValue(values[k], c.def.format ?? ''), align: c.def.align ?? (typeof values[k] === 'number' ? 'right' : 'left'), value: values[k] })),
      });
    }
  };
  if (rows.length) emit(rows, 0, '');
  if (it.grandTotal && rows.length) out.push(totalRow('grand', 'Total', rows, 0));
  return { columns: cols, rows: out, labelAt, sorting, warnings };
}
