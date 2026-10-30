// Copyright (c) 2026 Philipp Orlov

// Sub-reports: a body item that runs another report and lays it out in place, once or once per row of a dataset. The page
// lays out synchronously, so what the sub-reports need is fetched beforehand: `resolveSubreports` walks the report, loads each
// target, runs its datasets with the parameters the item gives, and does the same for the sub-reports of those.
// Free of Angular imports on purpose.

import { BodyItem, defaultParams, paramValue, ReportDef, SubreportItem } from './definition';
import { interpolate } from './expr';
import { Value } from './format';
import { context, DataSet, Env } from './scope';

export interface SubRun {
  report: ReportDef;
  params: Record<string, Value>;
  datasets: Record<string, DataSet>;
  /** The runs of the sub-reports this one holds. */
  subs: SubRuns;
}
export type SubRuns = Record<string, SubRun>;

export const MAX_SUB_DEPTH = 3;
export const MAX_SUB_RUNS = 60;
export const DEFAULT_REPEATS = 12;

/** The key a run is stored under: the report and its parameter values. */
export function subKey(report: string, params: Record<string, Value>): string {
  return report + '\u0000' + JSON.stringify(Object.keys(params).sort().map(k => [k, params[k]]));
}

/** Every item of `items`, columns included (not the bodies of sub-reports). */
export function forEachItem(items: BodyItem[], visit: (item: BodyItem) => void): void {
  for (const it of items) {
    visit(it);
    if (it.type === 'columns') for (const c of it.columns) forEachItem(c.items ?? [], visit);
  }
}

/** The parameter values for one run of `target`: what the item maps, else the same-named parameter of the parent, else the default. */
export function subParams(item: SubreportItem, target: ReportDef, parent: Env, ds: DataSet | null, row: Value[] | null): Record<string, Value> {
  const cx = context(parent, ds, row ? [row] : [], row);
  const defaults = defaultParams(target);
  const out: Record<string, Value> = {};
  for (const p of target.parameters) {
    const template = item.params?.[p.name];
    if (template !== undefined) out[p.name] = paramValue(p, interpolate(template, cx));
    else if (p.name in parent.params) out[p.name] = parent.params[p.name];
    else out[p.name] = defaults[p.name] ?? null;
  }
  return out;
}

/** The parameter sets an item runs its report with: one, or one per row of its dataset (up to the limit). */
export function subCalls(item: SubreportItem, target: ReportDef, parent: Env, datasets: Record<string, DataSet>): { params: Record<string, Value>; row: Value[] | null; ds: DataSet | null }[] {
  if (!item.dataset) return [{ params: subParams(item, target, parent, null, null), row: null, ds: null }];
  const ds = datasets[item.dataset];
  if (!ds) return [];
  const limit = Math.min(Math.max(item.limit ?? DEFAULT_REPEATS, 1), 50);
  return ds.rows.slice(0, limit).map(row => ({ params: subParams(item, target, parent, ds, row), row, ds }));
}

export interface SubDeps {
  /** The report of that name, or null. */
  load(name: string): Promise<ReportDef | null>;
  /** Runs the datasets of a report. */
  run(report: ReportDef, params: Record<string, Value>): Promise<{ datasets: Record<string, DataSet>; warnings: string[] }>;
}

export interface SubResolution {
  subs: SubRuns;
  warnings: string[];
}

/** Loads and runs everything the sub-reports of `report` need. A target that is missing or fails is a warning, not an error. */
export async function resolveSubreports(report: ReportDef, params: Record<string, Value>, datasets: Record<string, DataSet>, deps: SubDeps, now = new Date(), path: string[] = [report.name], budget = { runs: 0 }): Promise<SubResolution> {
  const subs: SubRuns = {};
  const warnings: string[] = [];
  const env: Env = { report, params, now, page: 1, pageCount: 0 };
  const items: SubreportItem[] = [];
  forEachItem(report.body, it => it.type === 'subreport' && items.push(it));
  const loaded = new Map<string, ReportDef | null>();
  for (const item of items) {
    if (path.length > MAX_SUB_DEPTH) {
      warnings.push(`sub-report ${item.report}: reports are nested more than ${MAX_SUB_DEPTH} deep`);
      continue;
    }
    if (path.includes(item.report)) {
      warnings.push(`sub-report ${item.report} contains itself (${[...path, item.report].join(' > ')})`);
      continue;
    }
    if (!loaded.has(item.report)) loaded.set(item.report, await deps.load(item.report).catch(() => null));
    const target = loaded.get(item.report);
    if (!target) {
      warnings.push(`sub-report ${item.report} does not exist`);
      continue;
    }
    const calls = subCalls(item, target, env, datasets);
    const wanted = item.dataset ? Math.min(Math.max(item.limit ?? DEFAULT_REPEATS, 1), 50) : 1;
    if (item.dataset && datasets[item.dataset]?.rows.length > wanted) warnings.push(`sub-report ${item.report}: only the first ${wanted} of ${datasets[item.dataset].rows.length} rows are repeated`);
    for (const call of calls) {
      const key = subKey(item.report, call.params);
      if (subs[key]) continue;
      if (budget.runs >= MAX_SUB_RUNS) {
        warnings.push(`more than ${MAX_SUB_RUNS} sub-report runs: the rest are left out`);
        break;
      }
      budget.runs++;
      try {
        const ran = await deps.run(target, call.params);
        warnings.push(...ran.warnings);
        const inner = await resolveSubreports(target, call.params, ran.datasets, deps, now, [...path, target.name], budget);
        warnings.push(...inner.warnings);
        subs[key] = { report: target, params: call.params, datasets: ran.datasets, subs: inner.subs };
      } catch (e) {
        warnings.push(`sub-report ${item.report}: ${(e as Error).message}`);
      }
    }
  }
  return { subs, warnings: [...new Set(warnings)] };
}
