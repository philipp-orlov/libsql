// Copyright (c) 2026 Philipp Orlov

// A report definition: what the designer edits, the server stores (as JSON, one file per report) and the runner runs.
// Free of Angular imports on purpose.

import { checkTemplate, parseExpression } from './expr';
import { checkRich } from './richtext';
import { isDatePattern } from './format';
import { bindNames } from './sql-params';

export const REPORT_FORMAT = 1;

export type ParamType = 'text' | 'integer' | 'real' | 'date' | 'boolean';

export interface ParamOption {
  value: string | number;
  label?: string;
}

export interface ParamDef {
  /** Referenced in dataset SQL as :name. */
  name: string;
  label?: string;
  type: ParamType;
  default?: string | number | boolean | null;
  /** A value is needed before the report can run. */
  required?: boolean;
  /** A fixed list to choose from. */
  options?: ParamOption[];
  /** Or the choices come from a query: first column the value, second (optional) the label. */
  optionsSql?: string;
}

export interface DatasetDef {
  name: string;
  /** One SELECT; `:param` names report parameters. */
  sql: string;
}

export interface Style {
  face?: 'regular' | 'bold';
  size?: number;
  color?: string;
  align?: 'left' | 'center' | 'right';
  background?: string;
}

export interface TextItem {
  type: 'text';
  /** With `{{ expression }}` parts. */
  text: string;
  style?: Style;
  /** Aggregates in the text (Sum(Amount)) run over this dataset's rows. */
  dataset?: string;
  /**
   * Rich text: the text may carry `[b]bold[/b]`, `[u]underline[/u]`, `[color=#e15759]color[/color]`, `[size=24]size[/size]`
   * and `[bg=#fff3cd]highlight[/bg]`, nested as you like (`[/]` closes the innermost), and lines that start with `- ` are bullets.
   */
  rich?: boolean;
}

/** Clicking a mark, a row or a cell opens another report. */
export interface DrillDef {
  /** The target report's name. */
  report: string;
  /**
   * What its parameters get, by name: a text with `{{ }}` parts that sees the clicked row (a chart: the row of the dataset
   * behind the mark; a table: the row; a matrix: the row, column and value fields). Parameters left out keep the target's default.
   */
  params?: Record<string, string>;
}

export interface ColumnDef {
  /** A column of the dataset; or leave empty and give an expression. */
  field?: string;
  expression?: string;
  header?: string;
  /** Share of the table's width (default 1). */
  width?: number;
  align?: 'left' | 'center' | 'right';
  /** A number pattern (`#,##0.00`) or a date pattern (`yyyy-MM-dd`). */
  format?: string;
  /** Shown in subtotal and total rows. */
  total?: 'sum' | 'avg' | 'min' | 'max' | 'count';
}

export interface GroupDef {
  field: string;
  /** The group's heading, with `{{ }}` parts; default `{{field}}`. */
  header?: string;
  sort?: 'asc' | 'desc';
  subtotals?: boolean;
  pageBreak?: boolean;
}

export interface TableItem {
  type: 'table';
  dataset: string;
  columns: ColumnDef[];
  groups?: GroupDef[];
  grandTotal?: boolean;
  zebra?: boolean;
  style?: Style;
  /** A click on a row opens the target report. */
  drill?: DrillDef;
}

export interface SeriesDef {
  field: string;
  label?: string;
  color?: string;
}

export interface ChartItem {
  type: 'chart';
  chart: 'bar' | 'line' | 'area' | 'pie';
  /** Bars: pile the series on each other. */
  stacked?: boolean;
  /** Bars: lie down, which suits long category names. */
  horizontal?: boolean;
  /** Pie: with a hole and the total in it. */
  donut?: boolean;
  /** Write the values on the bars, points and slices. */
  labels?: boolean;
  dataset: string;
  /** The column that names each bar, point or slice. */
  category: string;
  series: SeriesDef[];
  title?: string;
  height: number;
  legend?: boolean;
  /** A number pattern for the axis and labels. */
  format?: string;
  /** A click on a bar, point or slice opens the target report. */
  drill?: DrillDef;
}

export type Aggregate = 'sum' | 'avg' | 'min' | 'max' | 'count';
/** Color scales of a matrix: `blue` ... are white to that hue, `diverging` runs red - white - blue about the middle value. */
export type HeatScale = 'none' | 'blue' | 'green' | 'orange' | 'red' | 'purple' | 'teal' | 'diverging';

/** A cross-tab: one field spread over the columns, one or two down the rows, one measure in the cells. */
export interface MatrixItem {
  type: 'matrix';
  dataset: string;
  /** One or two fields down the side (the second nests in the first). */
  rows: string[];
  /** The field whose values become the column headings. */
  column: string;
  /** The measure in the cells. */
  value: string;
  agg?: Aggregate;
  /** A number pattern for the cells. */
  format?: string;
  title?: string;
  heat?: HeatScale;
  /** Totals at the right end of each row and at the bottom of each column. */
  rowTotals?: boolean;
  columnTotals?: boolean;
  /** With two row fields: a subtotal line under each group of the first. */
  subtotals?: boolean;
  /** The order of the headings: as the data comes (default), ascending, descending, or (rows) by their total, largest first. */
  sortRows?: 'data' | 'asc' | 'desc' | 'total';
  sortColumns?: 'data' | 'asc' | 'desc';
  /** The corner cell; default the names of the row fields. */
  corner?: string;
  style?: Style;
  /** A click on a cell opens the target report. */
  drill?: DrillDef;
}

/** A picture: `src` is a `data:image/png|jpeg|gif|webp|svg+xml;base64,...` URI (it may hold `{{ }}` parts that read a text field). */
export interface ImageItem {
  type: 'image';
  src: string;
  /** Where `{{ }}` in `src` and `caption` read their rows. */
  dataset?: string;
  /** Points. With only one of width and height given the picture keeps its proportions; with neither it has its own size, shrunk to fit the page. */
  width?: number;
  height?: number;
  /** Stretch to the whole width of the page (or column). */
  fitWidth?: boolean;
  align?: 'left' | 'center' | 'right';
  caption?: string;
}

/** Side-by-side columns: each holds a list of items, laid out in its own width. */
export interface ColumnCell {
  /** Relative width (default 1). */
  width?: number;
  items: BodyItem[];
  /** Draw it as a white card with a thin border. */
  card?: boolean;
  /** A colored bar along the top of the card. */
  accent?: string;
  /** The card's fill (default white). */
  background?: string;
}
export interface ColumnsItem {
  type: 'columns';
  columns: ColumnCell[];
  /** Points between the columns (default 12). */
  gap?: number;
}

/** Another report, run and laid out right here; once, or once per row of a dataset. */
export interface SubreportItem {
  type: 'subreport';
  /** The other report's name. */
  report: string;
  /** Its parameters: `{{ }}` texts reading the row (when repeated). Parameters left out take the one of the same name here, else the default. */
  params?: Record<string, string>;
  /** Repeat for each row of this dataset. */
  dataset?: string;
  /** At most this many repeats (default 12, up to 50). */
  limit?: number;
  /** Start every repeat on a new page. */
  pageBreak?: boolean;
}

export interface RuleItem {
  type: 'rule';
}
export interface SpacerItem {
  type: 'spacer';
  height: number;
}
export interface PageBreakItem {
  type: 'pageBreak';
}

export type BodyItem = TextItem | TableItem | ChartItem | MatrixItem | ImageItem | ColumnsItem | SubreportItem | RuleItem | SpacerItem | PageBreakItem;
/** What a page header or footer may hold. */
export type BandItem = TextItem | ImageItem | RuleItem | SpacerItem;

export type PageSize = 'A3' | 'A4' | 'A5' | 'Letter' | 'Legal';

export interface PageDef {
  size: PageSize;
  orientation: 'portrait' | 'landscape';
  /** Points (1/72 inch). */
  margins: { top: number; right: number; bottom: number; left: number };
}

export interface ReportDef {
  format: typeof REPORT_FORMAT;
  /** The file name too: letters, digits, space, dash, underscore, dot. */
  name: string;
  title?: string;
  description?: string;
  page: PageDef;
  parameters: ParamDef[];
  datasets: DatasetDef[];
  header: BandItem[];
  footer: BandItem[];
  body: BodyItem[];
}

export const PAGE_SIZES: Record<PageSize, [number, number]> = {
  A3: [841.89, 1190.55],
  A4: [595.28, 841.89],
  A5: [419.53, 595.28],
  Letter: [612, 792],
  Legal: [612, 1008],
};

export function pageDimensions(page: PageDef): [number, number] {
  const [w, h] = PAGE_SIZES[page.size] ?? PAGE_SIZES.A4;
  return page.orientation === 'landscape' ? [h, w] : [w, h];
}

export function newReport(name = 'New report'): ReportDef {
  return {
    format: REPORT_FORMAT,
    name,
    title: name,
    description: '',
    page: { size: 'A4', orientation: 'portrait', margins: { top: 48, right: 40, bottom: 48, left: 40 } },
    parameters: [],
    datasets: [],
    header: [{ type: 'text', text: '{{ReportName}}', style: { face: 'bold', size: 10, color: '#555555' } }, { type: 'rule' }],
    footer: [{ type: 'rule' }, { type: 'text', text: 'Page {{PageNumber}} of {{PageCount}}', style: { size: 9, color: '#555555', align: 'right' } }],
    body: [{ type: 'text', text: '{{ReportName}}', style: { face: 'bold', size: 20 } }],
  };
}

const NAME = /^[A-Za-z0-9][A-Za-z0-9 _.-]{0,79}$/;

export function isValidReportName(name: string): boolean {
  return NAME.test(name) && !name.endsWith('.') && !name.includes('..');
}

/** Everything wrong with `r`, each as a sentence; empty when it can run. */
export function validateReport(r: ReportDef): string[] {
  const errors: string[] = [];
  const err = (s: string): void => void errors.push(s);
  if (r.format !== REPORT_FORMAT) err(`unsupported report format ${String(r.format)}`);
  if (!isValidReportName(r.name ?? '')) err('the name must be 1 to 80 letters, digits, spaces, dashes, underscores or dots');
  const m = r.page?.margins;
  if (!r.page || !(r.page.size in PAGE_SIZES) || !m) err('the page setup is incomplete');
  else {
    for (const side of ['top', 'right', 'bottom', 'left'] as const) {
      if (!(m[side] >= 0 && m[side] <= 200)) err(`the ${side} margin must be between 0 and 200 points`);
    }
  }
  const names = new Set<string>();
  for (const p of r.parameters ?? []) {
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(p.name ?? '')) err(`parameter name '${p.name}' is not an identifier`);
    else if (names.has(p.name)) err(`parameter ${p.name} is defined twice`);
    names.add(p.name);
    if (!['text', 'integer', 'real', 'date', 'boolean'].includes(p.type)) err(`parameter ${p.name}: unknown type ${p.type}`);
  }
  const sets = new Set<string>();
  for (const d of r.datasets ?? []) {
    // a dataset's name is only a label the items refer to, so it may have spaces and dashes
    if (!/^[A-Za-z_][A-Za-z0-9_ .-]{0,59}$/.test(d.name ?? '')) err(`dataset name '${d.name}' must be 1 to 60 letters, digits, spaces, dashes, dots or underscores, not starting with a digit`);
    else if (sets.has(d.name)) err(`dataset ${d.name} is defined twice`);
    sets.add(d.name);
    if (!d.sql?.trim()) err(`dataset ${d.name} has no SQL`);
    else for (const used of bindNames(d.sql).names) if (!names.has(used)) err(`dataset ${d.name} uses :${used}, which is not a parameter`);
  }
  const needDataset = (what: string, name: string): void => {
    if (!sets.has(name)) err(`${what} uses dataset '${name}', which does not exist`);
  };
  const template = (what: string, text: string): void => {
    const e = checkTemplate(text ?? '');
    if (e) err(`${what}: ${e}`);
  };
  const image = (at: string, it: ImageItem): void => {
    if (!it.src?.trim()) err(`${at} has no picture`);
    else if (!/\{\{/.test(it.src) && !/^data:image\/(png|jpeg|jpg|gif|webp|svg\+xml)[;,]/i.test(it.src)) err(`${at}: the picture must be a data:image/ URI (png, jpeg, gif, webp or svg)`);
    if (it.src) template(at, it.src);
    if (it.caption) template(at, it.caption);
    if (it.dataset) needDataset(at, it.dataset);
    for (const k of ['width', 'height'] as const) if (it[k] !== undefined && !(it[k]! >= 4 && it[k]! <= 2000)) err(`${at}: the ${k} must be between 4 and 2000 points`);
  };
  const drill = (at: string, d: DrillDef | undefined): void => {
    if (!d) return;
    if (!d.report?.trim()) err(`${at}: the drill-through has no target report`);
    for (const v of Object.values(d.params ?? {})) template(`${at} drill-through`, String(v));
  };
  const band = (where: string, items: BandItem[]): void => {
    for (const it of items ?? []) {
      if (it.type === 'text') template(`${where} text`, it.text);
      else if (it.type === 'image') image(`${where} image`, it);
      else if (it.type !== 'rule' && it.type !== 'spacer') err(`${where} may hold text, images, rules and spacers only`);
    }
  };
  band('the page header', r.header);
  band('the page footer', r.footer);
  const item = (it: BodyItem, at: string, depth: number): void => {
    switch (it.type) {
      case 'text':
        template(at, it.text);
        if (it.dataset) needDataset(at, it.dataset);
        if (it.rich) {
          const e = checkRich(it.text);
          if (e) err(`${at}: ${e}`);
        }
        break;
      case 'table':
        needDataset(at, it.dataset);
        if (!it.columns?.length) err(`${at} (table) has no columns`);
        for (const c of it.columns ?? []) {
          if (!c.field && !c.expression) err(`${at}: a column has neither a field nor an expression`);
          if (c.expression)
            try {
              parseExpression(c.expression);
            } catch (e) {
              err(`${at}: expression '${c.expression}': ${(e as Error).message}`);
            }
          if (c.format && !isDatePattern(c.format) && !/[#0]/.test(c.format)) err(`${at}: format '${c.format}' has no # or 0`);
        }
        for (const g of it.groups ?? []) {
          if (!g.field) err(`${at}: a group has no field`);
          template(`${at} group heading`, g.header ?? '');
        }
        drill(at, it.drill);
        break;
      case 'chart':
        if (!['bar', 'line', 'area', 'pie'].includes(it.chart)) err(`${at} (chart) has an unknown kind ${String(it.chart)}`);
        needDataset(at, it.dataset);
        if (!it.category) err(`${at} (chart) has no category column`);
        if (!it.series?.length) err(`${at} (chart) has no series`);
        if (!(it.height >= 80 && it.height <= 800)) err(`${at} (chart) height must be between 80 and 800 points`);
        drill(at, it.drill);
        break;
      case 'matrix':
        needDataset(at, it.dataset);
        if (!(it.rows?.length >= 1 && it.rows.length <= 2) || it.rows.some(f => !f)) err(`${at} (matrix) needs one or two row fields`);
        if (!it.column) err(`${at} (matrix) has no column field`);
        if (!it.value) err(`${at} (matrix) has no value field`);
        if (it.agg && !['sum', 'avg', 'min', 'max', 'count'].includes(it.agg)) err(`${at} (matrix): unknown aggregate ${it.agg}`);
        if (it.format && !isDatePattern(it.format) && !/[#0]/.test(it.format)) err(`${at}: format '${it.format}' has no # or 0`);
        drill(at, it.drill);
        break;
      case 'image':
        image(at, it);
        break;
      case 'columns':
        if (depth >= 2) err(`${at}: columns can be nested two deep at most`);
        if (!it.columns?.length || it.columns.length > 6) err(`${at} (columns) needs 1 to 6 columns`);
        it.columns?.forEach((c, k) => {
          if (c.width !== undefined && !(c.width > 0)) err(`${at}, column ${k + 1}: the width must be positive`);
          (c.items ?? []).forEach((sub, j) => {
            if (sub.type === 'pageBreak') err(`${at}, column ${k + 1}: a page break cannot go inside columns`);
            else item(sub, `${at}, column ${k + 1}, item ${j + 1}`, depth + 1);
          });
        });
        break;
      case 'subreport':
        if (!it.report?.trim()) err(`${at} (sub-report) has no report`);
        else if (it.report === r.name) err(`${at}: a report cannot contain itself`);
        if (it.dataset) needDataset(at, it.dataset);
        for (const v of Object.values(it.params ?? {})) template(`${at} (sub-report)`, String(v));
        if (it.limit !== undefined && !(it.limit >= 1 && it.limit <= 50)) err(`${at} (sub-report): the limit must be between 1 and 50`);
        break;
      case 'spacer':
        if (!(it.height >= 0 && it.height <= 400)) err(`${at} (spacer) height must be between 0 and 400 points`);
        break;
      case 'rule':
      case 'pageBreak':
        break;
      default:
        err(`${at} has an unknown type`);
    }
  };
  r.body?.forEach((it, i) => item(it, `body item ${i + 1}`, 0));
  return errors;
}

/** A parsed JSON file as a report, or throws with why not. Unknown extra members are dropped. */
export function parseReport(json: string): ReportDef {
  let raw: unknown;
  try {
    raw = JSON.parse(json);
  } catch (e) {
    throw new Error(`not valid JSON: ${(e as Error).message}`);
  }
  if (!raw || typeof raw !== 'object') throw new Error('a report is a JSON object');
  const base = newReport();
  const r = raw as Partial<ReportDef>;
  const merged: ReportDef = {
    ...base,
    ...r,
    page: { ...base.page, ...(r.page ?? {}), margins: { ...base.page.margins, ...(r.page?.margins ?? {}) } },
    parameters: r.parameters ?? [],
    datasets: r.datasets ?? [],
    header: r.header ?? [],
    footer: r.footer ?? [],
    body: r.body ?? [],
  };
  return merged;
}

/** The parameter values a report starts with: each parameter's default, or null. */
export function defaultParams(r: ReportDef): Record<string, string | number | null> {
  const out: Record<string, string | number | null> = {};
  for (const p of r.parameters) {
    const d = p.default;
    out[p.name] = d === undefined || d === null || d === '' ? (p.type === 'boolean' ? 0 : null) : typeof d === 'boolean' ? (d ? 1 : 0) : p.type === 'integer' ? Math.trunc(Number(d)) : p.type === 'real' ? Number(d) : typeof d === 'number' ? d : String(d);
  }
  return out;
}

/** What a parameter form holds as text, as the typed value the query binds: blank is null, a checkbox is 1 or 0. */
export function paramValue(p: ParamDef, text: string): string | number | null {
  const t = text.trim();
  if (p.type === 'boolean') return t === 'true' ? 1 : 0;
  if (t === '') return null;
  if (p.type === 'integer') return Number.isFinite(Number(t)) ? Math.trunc(Number(t)) : null;
  if (p.type === 'real') return Number.isFinite(Number(t)) ? Number(t) : null;
  return text; // text and date (yyyy-MM-dd)
}

/** A parameter's default as the text its form field starts with. */
export function paramText(p: ParamDef): string {
  const d = p.default;
  if (d === undefined || d === null) return '';
  return p.type === 'boolean' ? (d ? 'true' : '') : String(d);
}
