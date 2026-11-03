// Copyright (c) 2026 Philipp Orlov

// A chart item's data from its dataset. The drawing (bars, lines, areas, pies as primitives for the preview and the
// PDF) is @ui-kit/core/charts', re-exported here for the rest of the report engine. Free of Angular imports on purpose.

import { ChartData, PALETTE } from '@ui-kit/core/charts';
import { formatValue } from '@ui-kit/core/format';

import type { ChartItem } from './definition';
import { columnIndex, DataSet } from './scope';

export {
  drawChart,
  niceScale,
  PALETTE,
  type ChartBox,
  type ChartContext,
  type ChartData,
  type ChartKind,
  type ChartOptions,
  type ChartSeries,
  type ChartSpec,
  type Measurer,
} from '@ui-kit/core/charts';

/** The most rows a chart draws. */
export const CHART_MAX_ROWS = 200;

/** The categories and series a chart item draws from its dataset's rows (at most CHART_MAX_ROWS of them), and the rows they came from. */
export function chartData(it: ChartItem, ds: DataSet, label: string, warn: (message: string) => void): { data: ChartData; rows: DataSet['rows'] } | null {
  const cat = columnIndex(ds, it.category);
  if (cat < 0) {
    warn(`${label}: dataset ${it.dataset} has no column ${it.category}`);
    return null;
  }
  if (ds.rows.length > CHART_MAX_ROWS) warn(`${label}: only the first ${CHART_MAX_ROWS} of ${ds.rows.length} rows are drawn`);
  const rows = ds.rows.slice(0, CHART_MAX_ROWS);
  const series = it.series.map((s, i) => {
    const idx = columnIndex(ds, s.field);
    if (idx < 0) warn(`${label}: no column ${s.field}`);
    return {
      label: s.label ?? s.field,
      color: s.color ?? PALETTE[i % PALETTE.length],
      values: rows.map(r => {
        const v = idx >= 0 ? r[idx] : null;
        const x = typeof v === 'number' ? v : typeof v === 'string' && v.trim() !== '' ? Number(v) : NaN;
        return Number.isFinite(x) ? x : null;
      }),
    };
  });
  return { data: { categories: rows.map(r => formatValue(r[cat] ?? null)), series }, rows };
}
