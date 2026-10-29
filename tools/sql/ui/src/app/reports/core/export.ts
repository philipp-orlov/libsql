// Copyright (c) 2026 Philipp Orlov

// What a report's tables hold as data, for CSV: each table's rows in the order the report shows them (groups sorted),
// with the columns it shows (computed columns evaluated), unformatted, so a spreadsheet can use the numbers.
// Free of Angular imports on purpose.

import { ReportDef, TableItem } from './definition';
import { evaluate } from './expr';
import { Value } from './format';
import { context, DataSet, Env, ViewState } from './layout';
import { toCsv } from './csv';

export interface TableData {
  /** For the file name and the menu: `Table 2 (sales)`. */
  title: string;
  dataset: string;
  columns: string[];
  rows: Value[][];
}

function columnIndex(ds: DataSet, name: string): number {
  const exact = ds.columns.indexOf(name);
  return exact >= 0 ? exact : ds.columns.findIndex(c => c.toLowerCase() === name.toLowerCase());
}

export function tableData(report: ReportDef, params: Record<string, Value>, datasets: Record<string, DataSet>, now = new Date(), view?: ViewState): TableData[] {
  const env: Env = { report, params, now, page: 1, pageCount: 0 };
  const out: TableData[] = [];
  let n = 0;
  for (const [bi, item] of report.body.entries()) {
    n++;
    if (item.type !== 'table') continue;
    const t = item as TableItem;
    const ds = datasets[t.dataset];
    if (!ds) continue;
    const groups = (t.groups ?? []).map(g => ({ g, idx: columnIndex(ds, g.field) })).filter(g => g.idx >= 0);
    const cols = t.columns.map(c => ({ def: c, idx: c.field ? columnIndex(ds, c.field) : -1 }));
    const value = (c: (typeof cols)[number], row: Value[], scope: Value[][]): Value => {
      if (c.def.expression) {
        try {
          return evaluate(c.def.expression, context(env, ds, scope, row));
        } catch {
          return '#Error';
        }
      }
      return c.idx >= 0 ? row[c.idx] ?? null : null;
    };
    const compare = (x: Value, y: Value): number => (x === y ? 0 : x === null ? -1 : y === null ? 1 : typeof x === 'number' && typeof y === 'number' ? x - y : String(x).localeCompare(String(y)));
    // the order the report shows: by the groups, then by the column the reader sorted by
    const sorting = view?.sort?.[String(bi)];
    const sortCol = sorting ? cols[sorting.column] : undefined;
    let rows = ds.rows;
    if (groups.length || sortCol) {
      const keyed = rows.map(r => ({ r, k: sortCol ? value(sortCol, r, ds.rows) : null }));
      keyed.sort((a, b) => {
        for (const g of groups) {
          const c = compare(a.r[g.idx], b.r[g.idx]);
          if (c) return g.g.sort === 'desc' ? -c : c;
        }
        return sortCol ? compare(a.k, b.k) * (sorting!.dir === 'desc' ? -1 : 1) : 0;
      });
      rows = keyed.map(x => x.r);
    }
    out.push({
      title: `Table ${n} (${t.dataset})`,
      dataset: t.dataset,
      columns: cols.map(c => c.def.header ?? c.def.field ?? ''),
      rows: rows.map(row => cols.map(c => value(c, row, rows))),
    });
  }
  return out;
}

export function tableToCsv(t: TableData): string {
  return toCsv(t.columns, t.rows, { bom: true });
}
