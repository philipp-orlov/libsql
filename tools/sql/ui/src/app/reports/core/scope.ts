// Copyright (c) 2026 Philipp Orlov

// What items see while they are laid out or exported: the rows of a dataset, the report's parameters and the built-in
// names, as the context the expressions evaluate in. Free of Angular imports on purpose.

import { ExprContext } from './expr';
import { Value } from './format';
import type { ReportDef } from './definition';

export interface DataSet {
  columns: string[];
  rows: Value[][];
}

export interface Env {
  report: ReportDef;
  params: Record<string, Value>;
  now: Date;
  page: number;
  pageCount: number;
}

/** The context in which `row` (one of `rows`, both of `ds`) is evaluated; `ds` may be null for text without a dataset. */
export function context(env: Env, ds: DataSet | null, rows: Value[][], row: Value[] | null): ExprContext {
  const index = new Map<string, number>();
  ds?.columns.forEach((c, i) => {
    if (!index.has(c)) index.set(c, i);
    if (!index.has(c.toLowerCase())) index.set(c.toLowerCase(), i);
  });
  return {
    rows,
    row,
    lookup(name, r) {
      if (r) {
        const i = index.get(name) ?? index.get(name.toLowerCase());
        if (i !== undefined) return r[i] ?? null;
      }
      if (name in env.params) return env.params[name];
      switch (name) {
        case 'ReportName': return env.report.title || env.report.name;
        case 'Now': return env.now.toISOString();
        case 'PageNumber': return env.page;
        case 'PageCount': return env.pageCount || null;
      }
      return undefined;
    },
  };
}

/** The position of the column called `name` (case does not matter), or -1. */
export function columnIndex(ds: DataSet, name: string): number {
  const exact = ds.columns.indexOf(name);
  return exact >= 0 ? exact : ds.columns.findIndex(c => c.toLowerCase() === name.toLowerCase());
}

/** NULLs first, numbers by size, anything else as text. */
export function compareValues(x: Value, y: Value): number {
  return x === y ? 0 : x === null ? -1 : y === null ? 1 : typeof x === 'number' && typeof y === 'number' ? x - y : String(x).localeCompare(String(y));
}

/** A one-row dataset, for texts that read a single record (the row a drill-through was clicked on). */
export function recordOf(columns: string[], values: Value[]): { ds: DataSet; row: Value[] } {
  return { ds: { columns, rows: [values] }, row: values };
}
