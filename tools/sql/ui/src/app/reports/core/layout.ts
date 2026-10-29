// Copyright (c) 2026 Philipp Orlov

// Lays a report out: the definition, the parameter values and the rows of its datasets become pages of drawing
// primitives (primitives.ts). Text wraps, tables break across pages with their heading row repeated, groups get
// headings and subtotals, charts are drawn, the page header and footer are put on every page with its number.
// Free of Angular imports on purpose.

import { drawChart, ChartSpec, Measurer, PALETTE } from './chart';
import { BandItem, BodyItem, ChartItem, ColumnDef, pageDimensions, ReportDef, Style, TableItem, TextItem } from './definition';
import { evaluate, ExprContext, interpolate } from './expr';
import { formatValue, Value } from './format';
import { FontFace, PageDoc, Prim } from './primitives';
import { TrueTypeFont } from './ttf';

export interface DataSet {
  columns: string[];
  rows: Value[][];
}

/** What the reader changed in the preview: it is applied when the report is laid out, so the PDF follows it too. */
export interface ViewState {
  /** Per body item (its index, as text): the series (slices, for a pie) switched off in the legend. */
  hidden?: Record<string, number[]>;
  /** Per body item: the column a table is sorted by. */
  sort?: Record<string, { column: number; dir: 'asc' | 'desc' }>;
  /** The groups folded to their heading (and totals). */
  folded?: string[];
}

export interface LayoutInput {
  report: ReportDef;
  /** Parameter values by name. */
  params: Record<string, Value>;
  datasets: Record<string, DataSet>;
  fonts: Record<FontFace, TrueTypeFont>;
  now?: Date;
  /** Stop here (default 2000): a runaway query must not fill the browser's memory. */
  maxPages?: number;
  view?: ViewState;
}

export interface LayoutResult {
  pages: PageDoc[];
  warnings: string[];
}

const DEFAULT_STYLE: Required<Pick<Style, 'face' | 'size' | 'color' | 'align'>> = { face: 'regular', size: 10, color: '#000000', align: 'left' };
const PAD = 4; // cell padding
const LINE = 1.2; // line height as a multiple of the font size
const HEADER_FILL = '#eceff1';
const GROUP_FILL = '#f5f5f5';
const ZEBRA_FILL = '#fafafa';
const RULE = '#9e9e9e';

class Metrics implements Measurer {
  private readonly cache: Record<FontFace, Map<string, number>> = { regular: new Map(), bold: new Map() };
  constructor(private readonly fonts: Record<FontFace, TrueTypeFont>) {}
  width(text: string, face: FontFace, size: number): number {
    const c = this.cache[face];
    let w = c.get(text);
    if (w === undefined) {
      w = this.fonts[face].measure(text, 1);
      if (c.size > 50_000) c.clear();
      c.set(text, w);
    }
    return w * size;
  }
  /** Where the baseline of a line of `size` points that starts at `top` goes. */
  baseline(top: number, size: number): number {
    const f = this.fonts.regular;
    const asc = (f.ascent / f.unitsPerEm) * size;
    const desc = (-f.descent / f.unitsPerEm) * size;
    return top + (size * LINE - (asc + desc)) / 2 + asc;
  }
}

/** Splits text into lines that fit `width`. Words longer than a line are broken. */
function wrap(text: string, width: number, face: FontFace, size: number, m: Metrics): string[] {
  const out: string[] = [];
  for (const para of text.split(/\r?\n/)) {
    if (m.width(para, face, size) <= width) {
      out.push(para);
      continue;
    }
    let line = '';
    for (const word of para.split(/(?<= )/)) {
      const trial = line + word;
      if (m.width(trial.trimEnd(), face, size) <= width) {
        line = trial;
        continue;
      }
      if (line) out.push(line.trimEnd());
      line = '';
      let rest = word;
      while (m.width(rest.trimEnd(), face, size) > width && rest.length > 1) {
        let n = rest.length - 1;
        while (n > 1 && m.width(rest.slice(0, n), face, size) > width) n--;
        out.push(rest.slice(0, n));
        rest = rest.slice(n);
      }
      line = rest;
    }
    out.push(line.trimEnd());
  }
  return out;
}

export interface Env {
  report: ReportDef;
  params: Record<string, Value>;
  now: Date;
  page: number;
  pageCount: number;
}

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

class Flow {
  readonly pages: PageDoc[] = [];
  y = 0;
  constructor(
    readonly width: number,
    readonly height: number,
    readonly left: number,
    readonly top: number,
    readonly bottom: number, // the y where content must stop
    readonly contentWidth: number,
    private readonly maxPages: number,
    readonly onPage: (pageNumber: number) => void,
  ) {
    this.newPage();
  }
  full = false;
  private open: { section: 'header' | 'body' | 'footer'; index: number; y0: number } | null = null;
  /** The item being laid out starts here: its region (for selecting it in the preview) runs until end(). */
  begin(section: 'header' | 'body' | 'footer', index: number): void {
    this.open = { section, index, y0: this.y };
  }
  end(): void {
    this.close(this.y);
    this.open = null;
  }
  private close(y1: number): void {
    const o = this.open;
    const page = this.pages[this.pages.length - 1];
    if (o && page && y1 - o.y0 > 0.5) (page.regions ??= []).push({ section: o.section, index: o.index, x: this.left, y: o.y0, w: this.contentWidth, h: y1 - o.y0 });
  }
  get prims(): Prim[] {
    return this.pages[this.pages.length - 1].prims;
  }
  newPage(): void {
    if (this.pages.length >= this.maxPages) {
      this.full = true;
      return;
    }
    this.close(Math.min(this.y, this.bottom));
    this.pages.push({ width: this.width, height: this.height, prims: [] });
    this.y = this.top;
    if (this.open) this.open.y0 = this.top;
    this.onPage(this.pages.length);
  }
  /** Room for `h` more points here, else a new page. */
  ensure(h: number): void {
    if (this.y + h > this.bottom + 0.01 && this.y > this.top + 0.01) this.newPage();
  }
  add(p: Prim): void {
    if (!this.full) this.prims.push(p);
  }
}

export function layoutReport(input: LayoutInput): LayoutResult {
  const { report, fonts } = input;
  const warnings: string[] = [];
  const m = new Metrics(fonts);
  const [pw, ph] = pageDimensions(report.page);
  const mg = report.page.margins;
  const cw = pw - mg.left - mg.right;
  const env: Env = { report, params: input.params, now: input.now ?? new Date(), page: 1, pageCount: 0 };

  const style = (s?: Style): Required<Pick<Style, 'face' | 'size' | 'color' | 'align'>> & Pick<Style, 'background'> => ({ ...DEFAULT_STYLE, ...(s ?? {}) });

  // ---- text ---------------------------------------------------------------------------------------------
  /** The lines a text item shows. */
  const textLines = (it: TextItem, e: Env): { lines: string[]; st: ReturnType<typeof style> } => {
    const st = style(it.style);
    const ds = it.dataset ? input.datasets[it.dataset] ?? null : null;
    const cx = context(e, ds, ds?.rows ?? [], null);
    return { lines: wrap(interpolate(it.text, cx), cw - 2 * PAD, st.face, st.size, m), st };
  };
  const drawLine = (add: (p: Prim) => void, text: string, st: ReturnType<typeof style>, x: number, w: number, top: number): void => {
    if (st.background) add({ k: 'rect', x, y: top, w, h: st.size * LINE, fill: st.background });
    if (!text) return;
    const tw = m.width(text, st.face, st.size);
    const tx = st.align === 'right' ? x + w - PAD - tw : st.align === 'center' ? x + (w - tw) / 2 : x + PAD;
    add({ k: 'text', x: tx, y: m.baseline(top, st.size), text, face: st.face, size: st.size, color: st.color });
  };

  // ---- page header and footer ------------------------------------------------------------------------------
  const bandHeight = (items: BandItem[]): number => {
    let h = 0;
    for (const it of items) {
      if (it.type === 'text') {
        const { lines, st } = textLines(it, { ...env, page: 1, pageCount: 99 });
        h += lines.length * st.size * LINE + 2;
      } else if (it.type === 'rule') h += 6;
      else h += it.height;
    }
    return h;
  };
  const drawBand = (section: 'header' | 'footer', items: BandItem[], page: PageDoc, top: number, e: Env): void => {
    let y = top;
    for (const [index, it] of items.entries()) {
      const y0 = y;
      if (it.type === 'text') {
        const { lines, st } = textLines(it, e);
        for (const line of lines) {
          drawLine(p => page.prims.push(p), line, st, mg.left, cw, y);
          y += st.size * LINE;
        }
        y += 2;
      } else if (it.type === 'rule') {
        page.prims.push({ k: 'line', x1: mg.left, y1: y + 3, x2: mg.left + cw, y2: y + 3, color: RULE, lw: 0.75 });
        y += 6;
      } else y += it.height;
      (page.regions ??= []).push({ section, index, x: mg.left, y: y0, w: cw, h: y - y0 });
    }
  };
  const headerH = bandHeight(report.header);
  const footerH = bandHeight(report.footer);
  const top = mg.top + headerH;
  const bottom = ph - mg.bottom - footerH;
  if (bottom - top < 40) warnings.push('the margins, header and footer leave almost no room for the body');

  const flow = new Flow(pw, ph, mg.left, top, bottom, cw, input.maxPages ?? 2000, number => {
    env.page = number;
  });

  // ---- items ---------------------------------------------------------------------------------------------------
  const textItem = (it: TextItem): void => {
    const { lines, st } = textLines(it, env);
    const lh = st.size * LINE;
    for (const line of lines) {
      flow.ensure(lh);
      drawLine(p => flow.add(p), line, st, flow.left, flow.contentWidth, flow.y);
      flow.y += lh;
    }
    flow.y += 2;
  };

  const lookupDataset = (name: string, what: string): DataSet | null => {
    const ds = input.datasets[name];
    if (!ds) warnings.push(`${what}: dataset ${name} has no data`);
    return ds ?? null;
  };

  const columnIndex = (ds: DataSet, name: string): number => {
    const exact = ds.columns.indexOf(name);
    return exact >= 0 ? exact : ds.columns.findIndex(c => c.toLowerCase() === name.toLowerCase());
  };

  const table = (it: TableItem, n: number, bi: number): void => {
    const ds = lookupDataset(it.dataset, `table ${n}`);
    if (!ds) return;
    const cols = it.columns.map(c => ({ def: c, idx: c.field ? columnIndex(ds, c.field) : -1, numeric: false }));
    for (const c of cols) if (c.def.field && c.idx < 0) warnings.push(`table ${n}: dataset ${it.dataset} has no column ${c.def.field}`);
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
    }
    const total = cols.reduce((a, c) => a + (c.def.width ?? 1), 0) || 1;
    const widths = cols.map(c => (cw * (c.def.width ?? 1)) / total);
    const xs: number[] = [];
    widths.reduce((x, w) => (xs.push(x), x + w), flow.left);
    const body = style(it.style);

    const groups = (it.groups ?? []).map(g => ({ g, idx: columnIndex(ds, g.field) }));
    for (const g of groups) if (g.idx < 0) warnings.push(`table ${n}: no column ${g.g.field} to group by`);
    const used = groups.filter(g => g.idx >= 0);
    const cell = (c: (typeof cols)[number], row: Value[], scope: Value[][]): Value => {
      if (c.def.expression) {
        try {
          return evaluate(c.def.expression, context(env, ds, scope, row));
        } catch {
          return '#Error';
        }
      }
      return c.idx >= 0 ? row[c.idx] ?? null : null;
    };
    const align = (c: ColumnDef, v: Value): 'left' | 'center' | 'right' => c.align ?? (typeof v === 'number' ? 'right' : 'left');

    // The rows in the order shown: by the group fields, then (when the reader clicked a heading) by that column.
    const compare = (x: Value, y: Value): number => (x === y ? 0 : x === null ? -1 : y === null ? 1 : typeof x === 'number' && typeof y === 'number' ? x - y : String(x).localeCompare(String(y)));
    const sorting = input.view?.sort?.[String(bi)];
    const sortCol = sorting ? cols[sorting.column] : undefined;
    let rows = ds.rows;
    if (used.length || sortCol) {
      const keyed = rows.map(r => ({ r, k: sortCol ? cell(sortCol, r, ds.rows) : null }));
      keyed.sort((a, b) => {
        for (const g of used) {
          const c = compare(a.r[g.idx], b.r[g.idx]);
          if (c) return g.g.sort === 'desc' ? -c : c;
        }
        return sortCol ? compare(a.k, b.k) * (sorting!.dir === 'desc' ? -1 : 1) : 0;
      });
      rows = keyed.map(x => x.r);
    }
    const folded = new Set(input.view?.folded ?? []);
    const marker = (x: number, y: number, open: boolean): Prim =>
      open
        ? { k: 'path', cmds: [['M', x, y - 2], ['L', x + 6, y - 2], ['L', x + 3, y + 3], ['Z']], fill: '#546e7a' }
        : { k: 'path', cmds: [['M', x + 1, y - 3.5], ['L', x + 6, y], ['L', x + 1, y + 3.5], ['Z']], fill: '#546e7a' };

    const drawRow = (texts: { text: string; align: 'left' | 'center' | 'right' }[], face: FontFace, size: number, fill: string | undefined, indent = 0): { y: number; h: number } => {
      const lines = texts.map((t, i) => wrap(t.text, Math.max(widths[i] - 2 * PAD - (i === 0 ? indent : 0), 8), face, size, m));
      const h = Math.max(1, ...lines.map(l => l.length)) * size * LINE + 4;
      ensureRoom(h);
      if (fill) flow.add({ k: 'rect', x: flow.left, y: flow.y, w: flow.contentWidth, h, fill });
      texts.forEach((t, i) => {
        const x = xs[i] + (i === 0 ? indent : 0);
        const w = widths[i] - (i === 0 ? indent : 0);
        lines[i].forEach((line, k) => {
          const tw = m.width(line, face, size);
          const tx = t.align === 'right' ? x + w - PAD - tw : t.align === 'center' ? x + (w - tw) / 2 : x + PAD;
          if (line) flow.add({ k: 'text', x: tx, y: m.baseline(flow.y + 2 + k * size * LINE, size), text: line, face, size, color: body.color });
        });
      });
      const at = { y: flow.y, h };
      flow.y += h;
      return at;
    };
    const rule = (): void => flow.add({ k: 'line', x1: flow.left, y1: flow.y, x2: flow.left + flow.contentWidth, y2: flow.y, color: RULE, lw: 0.5 });
    const drawHeader = (): void => {
      const at = drawRowRaw(cols.map(c => ({ text: c.def.header ?? c.def.field ?? '', align: c.def.align ?? (c.numeric ? 'right' : 'left') })), 'bold', body.size, HEADER_FILL);
      cols.forEach((c, i) => {
        const active = sorting?.column === i;
        const rightAligned = (c.def.align ?? (c.numeric ? 'right' : 'left')) === 'right';
        if (active) {
          const ax = rightAligned ? xs[i] + 6 : xs[i] + widths[i] - 9;
          const cy = at.y + at.h / 2;
          flow.add({ k: 'path', cmds: sorting!.dir === 'asc' ? [['M', ax, cy + 2], ['L', ax + 6, cy + 2], ['L', ax + 3, cy - 3], ['Z']] : [['M', ax, cy - 2], ['L', ax + 6, cy - 2], ['L', ax + 3, cy + 3], ['Z']], fill: '#1976d2' });
        }
        flow.add({ k: 'rect', x: xs[i], y: at.y, w: widths[i], h: at.h, hit: { ghost: true, cls: 'th', action: { type: 'sort', item: bi, column: i }, tip: { rows: [{ label: active ? (sorting!.dir === 'asc' ? 'Click to sort descending' : 'Click to clear the sort') : 'Click to sort', value: c.def.header ?? c.def.field ?? '' }] } } });
      });
      flow.add({ k: 'line', x1: flow.left, y1: flow.y, x2: flow.left + flow.contentWidth, y2: flow.y, color: RULE, lw: 0.75 });
    };
    let inHeader = false;
    // A row of the heading is laid out without the page-break rule that would draw the heading again.
    const drawRowRaw = (texts: { text: string; align: 'left' | 'center' | 'right' }[], face: FontFace, size: number, fill?: string): { y: number; h: number } => {
      inHeader = true;
      const at = drawRow(texts, face, size, fill);
      inHeader = false;
      return at;
    };
    const ensureRoom = (h: number): void => {
      if (flow.y + h > flow.bottom + 0.01 && flow.y > flow.top + 0.01) {
        flow.newPage();
        if (!inHeader) drawHeader();
      }
    };

    // The label of a total goes in the first column that holds text and has no total of its own (a narrow rank or id
    // column in front of it would wrap the word).
    let labelAt = cols.findIndex(c => !c.def.total && !c.numeric);
    if (labelAt < 0) labelAt = cols.findIndex(c => !c.def.total);
    /** The totals of the columns that have one, as the text of each cell ('' for the others). */
    const totalTexts = (scope: Value[][]): { text: string; align: 'left' | 'center' | 'right' }[] =>
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
        return { text: formatValue(v, c.def.format ?? ''), align: c.def.align ?? 'right' };
      });
    const totalsRow = (label: string, scope: Value[][], indent: number): void => {
      const texts = totalTexts(scope).map((t, i) => (i === labelAt ? { text: label, align: 'left' as const } : t));
      rule();
      drawRow(texts, 'bold', body.size, undefined, indent);
    };

    let zebra = 0;
    const emit = (scope: Value[][], level: number, path: string): void => {
      if (flow.full) return;
      if (level < used.length) {
        const g = used[level];
        let first = true;
        for (let i = 0; i < scope.length; ) {
          let j = i + 1;
          while (j < scope.length && scope[j][g.idx] === scope[i][g.idx]) j++;
          const run = scope.slice(i, j);
          if (g.g.pageBreak && !first) flow.newPage();
          first = false;
          const cx = context(env, ds, run, run[0]);
          const heading = interpolate(g.g.header ?? `{{[${g.g.field}]}}`, cx);
          const id = `${bi}:${level}:${path}${String(run[0][g.idx])}`;
          const isFolded = folded.has(id);
          ensureRoom(body.size * LINE * 2 + 8);
          // a folded group shows its totals in its heading, so folding keeps the numbers
          const summary = isFolded && !g.g.subtotals ? totalTexts(run) : cols.map(() => ({ text: '', align: 'left' as const }));
          const at = drawRow([{ text: heading, align: 'left' }, ...summary.slice(1)], 'bold', body.size, GROUP_FILL, level * 10 + 9);
          flow.add(marker(flow.left + level * 10 + 4, at.y + at.h / 2, !isFolded));
          flow.add({ k: 'rect', x: flow.left, y: at.y, w: flow.contentWidth, h: at.h, hit: { ghost: true, cls: 'fold', action: { type: 'fold', item: bi, id }, tip: { rows: [{ label: isFolded ? 'Click to unfold' : 'Click to fold', value: `${run.length} row${run.length === 1 ? '' : 's'}` }] } } });
          if (!isFolded) emit(run, level + 1, `${path}${String(run[0][g.idx])}\u0001`);
          if (g.g.subtotals) totalsRow(`Total ${formatValue(run[0][g.idx] ?? null)}`, run, level * 10);
          i = j;
        }
        return;
      }
      for (const row of scope) {
        if (flow.full) return;
        const at = drawRow(
          cols.map(c => {
            const v = cell(c, row, scope);
            return { text: formatValue(v, c.def.format ?? ''), align: align(c.def, v) };
          }),
          body.face,
          body.size,
          it.zebra && zebra++ % 2 ? ZEBRA_FILL : undefined,
        );
        flow.add({ k: 'rect', x: flow.left, y: at.y, w: flow.contentWidth, h: at.h, hit: { ghost: true, cls: 'row' } });
      }
    };

    drawHeader();
    if (!rows.length) {
      drawRow([{ text: 'No rows', align: 'left' }, ...cols.slice(1).map(() => ({ text: '', align: 'left' as const }))], 'regular', body.size, undefined);
    } else emit(rows, 0, '');
    if (it.grandTotal && rows.length) totalsRow('Total', rows, 0);
    rule();
    flow.y += 6;
  };

  const chart = (it: ChartItem, n: number, bi: number): void => {
    const ds = lookupDataset(it.dataset, `chart ${n}`);
    if (!ds) return;
    const cat = columnIndex(ds, it.category);
    if (cat < 0) {
      warnings.push(`chart ${n}: dataset ${it.dataset} has no column ${it.category}`);
      return;
    }
    const MAX = 200;
    if (ds.rows.length > MAX) warnings.push(`chart ${n}: only the first ${MAX} of ${ds.rows.length} rows are drawn`);
    const rows = ds.rows.slice(0, MAX);
    const series = it.series.map((s, i) => {
      const idx = columnIndex(ds, s.field);
      if (idx < 0) warnings.push(`chart ${n}: no column ${s.field}`);
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
    flow.ensure(it.height);
    const spec: ChartSpec = { kind: it.chart, stacked: it.stacked, horizontal: it.horizontal, donut: it.donut, labels: it.labels };
    const hidden = input.view?.hidden?.[String(bi)] ?? [];
    const prims = drawChart(spec, { categories: rows.map(r => formatValue(r[cat] ?? null)), series }, { x: flow.left, y: flow.y, w: flow.contentWidth, h: it.height }, { title: it.title, legend: it.legend, format: it.format }, m, { item: bi, hidden });
    for (const p of prims) flow.add(p);
    flow.y += it.height + 8;
  };

  let n = 0;
  for (const it of report.body as BodyItem[]) {
    if (flow.full) break;
    n++;
    flow.begin('body', n - 1);
    switch (it.type) {
      case 'text': textItem(it); break;
      case 'table': table(it, n, n - 1); break;
      case 'chart': chart(it, n, n - 1); break;
      case 'rule':
        flow.ensure(6);
        flow.add({ k: 'line', x1: flow.left, y1: flow.y + 3, x2: flow.left + flow.contentWidth, y2: flow.y + 3, color: RULE, lw: 0.75 });
        flow.y += 6;
        break;
      case 'spacer': flow.y += it.height; break;
      case 'pageBreak': flow.newPage(); break;
    }
    flow.end();
  }
  if (flow.full) warnings.push(`the report was cut at ${flow.pages.length} pages`);

  // The header and footer need the number of pages.
  const count = flow.pages.length;
  flow.pages.forEach((page, i) => {
    const e = { ...env, page: i + 1, pageCount: count };
    drawBand('header', report.header, page, mg.top, e);
    drawBand('footer', report.footer, page, ph - mg.bottom - footerH, e);
  });
  return { pages: flow.pages, warnings };
}

