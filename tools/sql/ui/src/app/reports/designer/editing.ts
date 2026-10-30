// Copyright (c) 2026 Philipp Orlov

import { ReportDef } from '../core';

/** The editors change a report by emitting one of these: the designer applies it to a copy of the report. */
export type Edit = (report: ReportDef) => void;

/** Members that mean "not set" when emptied: they are removed from the definition instead of being kept as ''. */
const OPTIONAL = new Set(['format', 'header', 'width', 'align', 'color', 'background', 'label', 'title', 'description', 'dataset', 'expression', 'total', 'face', 'size', 'sort', 'optionsSql', 'default', 'accent', 'caption', 'src', 'width', 'height', 'limit', 'corner', 'agg', 'heat', 'sortRows', 'sortColumns']);

/** Sets `object.a.b.0.c = value` for the path 'a.b.0.c'; an emptied optional member is deleted. */
export function assign(object: unknown, path: string, value: unknown): void {
  const keys = path.split('.');
  let at = object as Record<string, unknown>;
  for (const key of keys.slice(0, -1)) {
    if (at[key] === undefined || at[key] === null) at[key] = {};
    at = at[key] as Record<string, unknown>;
  }
  const last = keys[keys.length - 1];
  if ((value === '' || value === undefined || value === null || (typeof value === 'number' && Number.isNaN(value))) && OPTIONAL.has(last)) delete at[last];
  else at[last] = value;
}

/** The list a path like `body`, `header` or `body.3.columns.1.items` leads to. */
export function listAt(report: ReportDef, path: string): unknown[] {
  let at: unknown = report;
  for (const key of path.split('.')) at = (at as Record<string, unknown>)[key];
  return at as unknown[];
}

/** Moves `list[from]` to `list[to]`. */
export function move<T>(list: T[], from: number, to: number): void {
  if (to < 0 || to >= list.length) return;
  const [item] = list.splice(from, 1);
  list.splice(to, 0, item);
}

/** A name not in `taken`: `base`, `base2`, `base3` ... */
export function unique(base: string, taken: string[]): string {
  if (!taken.includes(base)) return base;
  for (let n = 2; ; n++) if (!taken.includes(base + n)) return base + n;
}

/** The number in a text field, or undefined when it is blank or not one. */
export function numberOf(text: string): number | undefined {
  if (text.trim() === '') return undefined;
  const n = Number(text);
  return Number.isFinite(n) ? n : undefined;
}
