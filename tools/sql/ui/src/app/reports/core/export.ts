// Copyright (c) 2026 Philipp Orlov

// What a report's tables hold as data, for CSV: each table's rows in the order the report shows them (groups sorted),
// with the columns it shows (computed columns evaluated), unformatted, so a spreadsheet can use the numbers.
// Free of Angular imports on purpose.

import { buildBlocks, walkBlocks } from './blocks';
import { ReportDef } from './definition';
import { Value } from './format';
import { ViewState } from './layout';
import { DataSet } from './scope';
import { SubRuns } from './subreport';
import { toCsv } from './csv';

export interface TableData {
  /** For the file name and the menu: `Table 2 (sales)`. */
  title: string;
  dataset: string;
  columns: string[];
  rows: Value[][];
}

export function tableData(report: ReportDef, params: Record<string, Value>, datasets: Record<string, DataSet>, now = new Date(), view?: ViewState, subs?: SubRuns): TableData[] {
  const out: TableData[] = [];
  walkBlocks(buildBlocks({ report, params, datasets, subs, view, now }).blocks, b => {
    if (b.t !== 'table') return;
    out.push({
      title: b.title,
      dataset: b.item.dataset,
      columns: b.model.columns.map(c => c.header),
      rows: b.model.rows.filter(r => r.kind === 'data').map(r => r.values!),
    });
  });
  return out;
}

export function tableToCsv(t: TableData): string {
  return toCsv(t.columns, t.rows, { bom: true });
}
