// Copyright (c) 2026 Philipp Orlov

// Lays a report out: the definition, the parameter values and the rows of its datasets become pages of drawing
// primitives (primitives.ts). Text wraps (rich text too), tables break across pages with their heading row repeated, groups
// get headings and subtotals, charts and cross-tabs are drawn, pictures are placed, items stand side by side in columns,
// sub-reports are laid out in place, and the page header and footer are put on every page with its number.
// Free of Angular imports on purpose.

import { chartData, drawChart, ChartSpec, Measurer } from './chart';
import { BandItem, BodyItem, ChartItem, ColumnsItem, DrillDef, ImageItem, MatrixItem, pageDimensions, ReportDef, Style, SubreportItem, TableItem, TextItem } from './definition';
import { evaluate, interpolate } from './expr';
import { formatValue, Value } from './format';
import { imageInfo } from './image';
import { heatFill, heatRamp, inkOn, matrixModel } from './matrix';
import { Action, FontFace, PageDoc, Prim } from './primitives';
import { parseRich, wrapRich, BULLET_INDENT } from './richtext';
import { columnIndex, context, DataSet, Env } from './scope';
import { subCalls, SubRuns, subKey } from './subreport';
import { tableModel } from './tablemodel';
import { TrueTypeFont } from './ttf';

export * from './scope';

/** What the reader changed in the preview: it is applied when the report is laid out, so the PDF follows it too. */
export interface ViewState {
  /** Per body item (its id, as text): the series (slices, for a pie) switched off in the legend. */
  hidden?: Record<string, number[]>;
  /** Per body item: the column a table or cross-tab is sorted by. */
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
  /** The runs of the sub-reports (resolveSubreports). */
  subs?: SubRuns;
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
const INK = '#222222';
const MUTED = '#6b7280';
const CARD_BORDER = '#e0e0e0';
const CARD_PAD = 8;

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

/** `text` cut to `width` with an ellipsis. */
function fit(text: string, width: number, face: FontFace, size: number, m: Metrics): string {
  if (m.width(text, face, size) <= width) return text;
  let s = text;
  while (s.length > 1 && m.width(s + '…', face, size) > width) s = s.slice(0, -1);
  return s.length > 1 ? s + '…' : '';
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
    /** Where the first page starts (columns start in the middle of a page); later pages start at `top`. */
    private readonly firstY: number = top,
  ) {
    this.newPage();
  }
  full = false;
  private open: { section: 'header' | 'body' | 'footer'; index: number; list?: string; y0: number } | null = null;
  /** The item being laid out starts here: its region (for selecting it in the preview) runs until end(). */
  begin(section: 'header' | 'body' | 'footer', index: number, list?: string): void {
    this.open = { section, index, list, y0: this.y };
  }
  end(): void {
    this.close(this.y);
    this.open = null;
  }
  private close(y1: number): void {
    const o = this.open;
    const page = this.pages[this.pages.length - 1];
    if (o && page && y1 - o.y0 > 0.5) (page.regions ??= []).push({ section: o.section, index: o.index, ...(o.list ? { list: o.list } : {}), x: this.left, y: o.y0, w: this.contentWidth, h: y1 - o.y0 });
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
    this.y = this.pages.length === 1 ? this.firstY : this.top;
    if (this.open) this.open.y0 = this.y;
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

/** One report being laid out: the root, or a sub-report with its own parameters and data. */
interface Scope {
  report: ReportDef;
  env: Env;
  datasets: Record<string, DataSet>;
  subs: SubRuns;
}

/** A line of a text box: its height, and how to draw it at `x` in a box `w` wide whose top is `top`. */
interface TextLine {
  h: number;
  draw(add: (p: Prim) => void, x: number, w: number, top: number): void;
}

export function layoutReport(input: LayoutInput): LayoutResult {
  const { report, fonts } = input;
  const warnings: string[] = [];
  const m = new Metrics(fonts);
  const [pw, ph] = pageDimensions(report.page);
  const mg = report.page.margins;
  const cw = pw - mg.left - mg.right;
  const env: Env = { report, params: input.params, now: input.now ?? new Date(), page: 1, pageCount: 0 };
  const root: Scope = { report, env, datasets: input.datasets, subs: input.subs ?? {} };
  const view = input.view;

  const style = (s?: Style): Required<Pick<Style, 'face' | 'size' | 'color' | 'align'>> & Pick<Style, 'background'> => ({ ...DEFAULT_STYLE, ...(s ?? {}) });
  const warn = (w: string): void => void (warnings.includes(w) || warnings.push(w));

  // ---- text ---------------------------------------------------------------------------------------------------
  const drawPlain = (add: (p: Prim) => void, text: string, st: ReturnType<typeof style>, x: number, w: number, top: number): void => {
    if (st.background) add({ k: 'rect', x, y: top, w, h: st.size * LINE, fill: st.background });
    if (!text) return;
    const tw = m.width(text, st.face, st.size);
    const tx = st.align === 'right' ? x + w - PAD - tw : st.align === 'center' ? x + (w - tw) / 2 : x + PAD;
    add({ k: 'text', x: tx, y: m.baseline(top, st.size), text, face: st.face, size: st.size, color: st.color });
  };

  /** The lines a text item shows, in a box `width` wide. */
  const textLines = (it: TextItem, e: Env, datasets: Record<string, DataSet>, width: number): TextLine[] => {
    const st = style(it.style);
    const ds = it.dataset ? datasets[it.dataset] ?? null : null;
    const cx = context(e, ds, ds?.rows ?? [], null);
    if (!it.rich) {
      return wrap(interpolate(it.text, cx), width - 2 * PAD, st.face, st.size, m).map(text => ({ h: st.size * LINE, draw: (add, x, w, top) => drawPlain(add, text, st, x, w, top) }));
    }
    const paragraphs = parseRich(it.text, expr => interpolate('{{' + expr + '}}', cx));
    return wrapRich(paragraphs, width - 2 * PAD, { face: st.face, size: st.size, color: st.color }, m).map(line => {
      const h = line.size * LINE;
      return {
        h,
        draw: (add, x, w, top) => {
          if (st.background) add({ k: 'rect', x, y: top, w, h, fill: st.background });
          const base = m.baseline(top, line.size);
          const avail = w - 2 * PAD - line.indent;
          const free = Math.max(avail - line.width, 0);
          let fx = x + PAD + line.indent + (st.align === 'right' ? free : st.align === 'center' ? free / 2 : 0);
          if (line.bullet && line.fragments[0]) add({ k: 'text', x: x + PAD + Math.max(line.indent - BULLET_INDENT, 0), y: base, text: '•', face: 'bold', size: line.fragments[0].size, color: line.fragments[0].color });
          for (const f of line.fragments) {
            if (f.bg) add({ k: 'rect', x: fx - 1, y: base - f.size * 0.88, w: f.width + 2, h: f.size * 1.12, fill: f.bg });
            if (f.text) add({ k: 'text', x: fx, y: base, text: f.text, face: f.face, size: f.size, color: f.color });
            if (f.underline && f.text) add({ k: 'line', x1: fx, y1: base + f.size * 0.13, x2: fx + f.width, y2: base + f.size * 0.13, color: f.color, lw: Math.max(0.5, f.size / 16) });
            fx += f.width;
          }
        },
      };
    });
  };

  // ---- pictures -------------------------------------------------------------------------------------------------
  interface Placed {
    src: string;
    w: number;
    h: number;
    /** From the left of the box. */
    dx: number;
    caption: TextLine[];
  }
  const placeImage = (it: ImageItem, e: Env, datasets: Record<string, DataSet>, width: number, maxH: number): Placed | null => {
    const ds = it.dataset ? datasets[it.dataset] ?? null : null;
    const cx = context(e, ds, ds?.rows ?? [], null);
    const src = interpolate(it.src, cx).trim();
    if (!src) return null;
    const info = imageInfo(src);
    if (!info) {
      warn('an image is not a PNG, JPEG, GIF, WebP or SVG that this report can show');
      return null;
    }
    const ratio = info.width / info.height;
    let w = info.width * 0.75;
    let h = info.height * 0.75;
    if (it.fitWidth) {
      w = width;
      h = w / ratio;
    } else if (it.width && it.height) {
      w = it.width;
      h = it.height;
    } else if (it.width) {
      w = it.width;
      h = w / ratio;
    } else if (it.height) {
      h = it.height;
      w = h * ratio;
    }
    if (w > width) {
      h *= width / w;
      w = width;
    }
    if (h > maxH) {
      w *= maxH / h;
      h = maxH;
    }
    const align = it.align ?? 'left';
    const caption = it.caption ? textLines({ type: 'text', text: it.caption, dataset: it.dataset, style: { size: 8.5, color: MUTED, align } }, e, datasets, width) : [];
    return { src, w, h, dx: align === 'center' ? (width - w) / 2 : align === 'right' ? width - w : 0, caption };
  };

  // ---- page header and footer ------------------------------------------------------------------------------
  const bandItemHeight = (it: BandItem, e: Env): number => {
    if (it.type === 'text') return textLines(it, e, input.datasets, cw).reduce((a, l) => a + l.h, 0) + 2;
    if (it.type === 'image') {
      const p = placeImage(it, e, input.datasets, cw, 120);
      return p ? p.h + p.caption.reduce((a, l) => a + l.h, 0) + 2 : 0;
    }
    return it.type === 'rule' ? 6 : it.height;
  };
  const bandHeight = (items: BandItem[]): number => items.reduce((h, it) => h + bandItemHeight(it, { ...env, page: 1, pageCount: 99 }), 0);
  const drawBand = (section: 'header' | 'footer', items: BandItem[], page: PageDoc, top: number, e: Env): void => {
    let y = top;
    for (const [index, it] of items.entries()) {
      const y0 = y;
      const add = (p: Prim): void => void page.prims.push(p);
      if (it.type === 'text') {
        for (const line of textLines(it, e, input.datasets, cw)) {
          line.draw(add, mg.left, cw, y);
          y += line.h;
        }
        y += 2;
      } else if (it.type === 'image') {
        const p = placeImage(it, e, input.datasets, cw, 120);
        if (p) {
          add({ k: 'image', x: mg.left + p.dx, y, w: p.w, h: p.h, src: p.src });
          y += p.h;
          for (const line of p.caption) {
            line.draw(add, mg.left, cw, y);
            y += line.h;
          }
          y += 2;
        }
      } else if (it.type === 'rule') {
        add({ k: 'line', x1: mg.left, y1: y + 3, x2: mg.left + cw, y2: y + 3, color: RULE, lw: 0.75 });
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

  const flow0 = new Flow(pw, ph, mg.left, top, bottom, cw, input.maxPages ?? 2000, number => {
    env.page = number;
  });

  // ---- drill-through -------------------------------------------------------------------------------------------------
  /** What a click on the mark that stands for `row` does. */
  const drillAction = (d: DrillDef, sc: Scope, ds: DataSet, row: Value[]): Action => {
    const cx = context(sc.env, ds, [row], row);
    const params: Record<string, string> = {};
    for (const [k, v] of Object.entries(d.params ?? {})) params[k] = interpolate(String(v), cx);
    return { type: 'drill', report: d.report, params };
  };

  const dataset = (sc: Scope, name: string, what: string): DataSet | null => {
    const ds = sc.datasets[name];
    if (!ds) warn(`${what}: dataset ${name} has no data`);
    return ds ?? null;
  };

  // ---- items ---------------------------------------------------------------------------------------------------
  const textItem = (it: TextItem, sc: Scope, flow: Flow): void => {
    for (const line of textLines(it, sc.env, sc.datasets, flow.contentWidth)) {
      flow.ensure(line.h);
      line.draw(p => flow.add(p), flow.left, flow.contentWidth, flow.y);
      flow.y += line.h;
    }
    flow.y += 2;
  };

  const imageItem = (it: ImageItem, sc: Scope, flow: Flow): void => {
    const p = placeImage(it, sc.env, sc.datasets, flow.contentWidth, Math.max(flow.bottom - flow.top - 20, 40));
    if (!p) return;
    const capH = p.caption.reduce((a, l) => a + l.h, 0);
    flow.ensure(p.h + capH);
    flow.add({ k: 'image', x: flow.left + p.dx, y: flow.y, w: p.w, h: p.h, src: p.src });
    flow.y += p.h;
    for (const line of p.caption) {
      line.draw(q => flow.add(q), flow.left, flow.contentWidth, flow.y);
      flow.y += line.h;
    }
    flow.y += 6;
  };

  const table = (it: TableItem, sc: Scope, flow: Flow, id: number | string, label: string): void => {
    const ds = dataset(sc, it.dataset, label);
    if (!ds) return;
    const sid = String(id);
    const model = tableModel(it, ds, sc.env, { sort: view?.sort?.[sid], folded: view?.folded ?? [] }, sid, label);
    model.warnings.forEach(warn);
    const cols = model.columns;
    const sorting = model.sorting;
    const total = cols.reduce((a, c) => a + (c.def.width ?? 1), 0) || 1;
    const widths = cols.map(c => (flow.contentWidth * (c.def.width ?? 1)) / total);
    const xs: number[] = [];
    widths.reduce((x, w) => (xs.push(x), x + w), flow.left);
    const body = style(it.style);
    const marker = (x: number, y: number, open: boolean): Prim =>
      open
        ? { k: 'path', cmds: [['M', x, y - 2], ['L', x + 6, y - 2], ['L', x + 3, y + 3], ['Z']], fill: '#546e7a' }
        : { k: 'path', cmds: [['M', x + 1, y - 3.5], ['L', x + 6, y], ['L', x + 1, y + 3.5], ['Z']], fill: '#546e7a' };

    let inHeader = false;
    const ensureRoom = (h: number): void => {
      if (flow.y + h > flow.bottom + 0.01 && flow.y > flow.top + 0.01) {
        flow.newPage();
        if (!inHeader) drawHeader();
      }
    };
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
    // A row of the heading is laid out without the page-break rule that would draw the heading again.
    const drawRowRaw = (texts: { text: string; align: 'left' | 'center' | 'right' }[], face: FontFace, size: number, fill?: string): { y: number; h: number } => {
      inHeader = true;
      const at = drawRow(texts, face, size, fill);
      inHeader = false;
      return at;
    };
    const rule = (): void => flow.add({ k: 'line', x1: flow.left, y1: flow.y, x2: flow.left + flow.contentWidth, y2: flow.y, color: RULE, lw: 0.5 });
    const drawHeader = (): void => {
      const at = drawRowRaw(cols.map(c => ({ text: c.header, align: c.align })), 'bold', body.size, HEADER_FILL);
      cols.forEach((c, i) => {
        const active = sorting?.column === i;
        if (active) {
          const ax = c.align === 'right' ? xs[i] + 6 : xs[i] + widths[i] - 9;
          const cy = at.y + at.h / 2;
          flow.add({ k: 'path', cmds: sorting!.dir === 'asc' ? [['M', ax, cy + 2], ['L', ax + 6, cy + 2], ['L', ax + 3, cy - 3], ['Z']] : [['M', ax, cy - 2], ['L', ax + 6, cy - 2], ['L', ax + 3, cy + 3], ['Z']], fill: '#1976d2' });
        }
        flow.add({ k: 'rect', x: xs[i], y: at.y, w: widths[i], h: at.h, hit: { ghost: true, cls: 'th', action: { type: 'sort', item: id, column: i }, tip: { rows: [{ label: active ? (sorting!.dir === 'asc' ? 'Click to sort descending' : 'Click to clear the sort') : 'Click to sort', value: c.header }] } } });
      });
      flow.add({ k: 'line', x1: flow.left, y1: flow.y, x2: flow.left + flow.contentWidth, y2: flow.y, color: RULE, lw: 0.75 });
    };

    drawHeader();
    if (!model.rows.length) {
      drawRow([{ text: 'No rows', align: 'left' }, ...cols.slice(1).map(() => ({ text: '', align: 'left' as const }))], 'regular', body.size, undefined);
    }
    let zebra = 0;
    for (const row of model.rows) {
      if (flow.full) break;
      if (row.kind === 'group') {
        if (row.pageBreak) flow.newPage();
        ensureRoom(body.size * LINE * 2 + 8);
        const at = drawRow(row.cells, 'bold', body.size, GROUP_FILL, row.level * 10 + 9);
        flow.add(marker(flow.left + row.level * 10 + 4, at.y + at.h / 2, !row.folded));
        flow.add({ k: 'rect', x: flow.left, y: at.y, w: flow.contentWidth, h: at.h, hit: { ghost: true, cls: 'fold', action: { type: 'fold', item: id, id: row.id! }, tip: { rows: [{ label: row.folded ? 'Click to unfold' : 'Click to fold', value: `${row.count} row${row.count === 1 ? '' : 's'}` }] } } });
      } else if (row.kind === 'data') {
        const at = drawRow(row.cells, body.face, body.size, it.zebra && zebra++ % 2 ? ZEBRA_FILL : undefined);
        flow.add({
          k: 'rect', x: flow.left, y: at.y, w: flow.contentWidth, h: at.h,
          hit: it.drill
            ? { ghost: true, cls: 'row', action: drillAction(it.drill, sc, ds, row.source!), tip: { rows: [{ label: 'Click to open', value: it.drill.report }] } }
            : { ghost: true, cls: 'row' },
        });
      } else {
        rule();
        drawRow(row.cells, 'bold', body.size, undefined, row.kind === 'grand' ? 0 : row.level * 10);
      }
    }
    rule();
    flow.y += 6;
  };

  const chart = (it: ChartItem, sc: Scope, flow: Flow, id: number | string, label: string): void => {
    const ds = dataset(sc, it.dataset, label);
    if (!ds) return;
    const drawn = chartData(it, ds, label, warn);
    if (!drawn) return;
    const { data, rows } = drawn;
    flow.ensure(it.height);
    const spec: ChartSpec = { kind: it.chart, stacked: it.stacked, horizontal: it.horizontal, donut: it.donut, labels: it.labels };
    const hidden = view?.hidden?.[String(id)] ?? [];
    const drill = it.drill ? { action: (c: number): Action => drillAction(it.drill!, sc, ds, rows[c]), label: it.drill.report } : undefined;
    const prims = drawChart(spec, data, { x: flow.left, y: flow.y, w: flow.contentWidth, h: it.height }, { title: it.title, legend: it.legend, format: it.format }, m, { item: id, hidden, drill });
    for (const p of prims) flow.add(p);
    flow.y += it.height + 8;
  };

  const matrix = (it: MatrixItem, sc: Scope, flow: Flow, id: number | string, label: string): void => {
    const ds = dataset(sc, it.dataset, label);
    if (!ds) return;
    const sid = String(id);
    const size = it.style?.size ?? 9;
    const W = flow.contentWidth;
    const fmt = it.format ?? '#,##0.##';
    const sorting = view?.sort?.[sid];
    // the room for the row headings follows the longest of them
    const probe = matrixModel({ ...it, rowTotals: false }, ds, label, 1000, undefined);
    const labelW = Math.min(Math.max(...probe.rows.map(r => m.width(r.label, r.kind === 'data' ? 'regular' : 'bold', size) + (r.level ? 10 : 0)), m.width(probe.corner, 'bold', size - 1), 40) + 14, W * 0.34);
    const extra = it.rowTotals ? 1 : 0;
    const model = matrixModel(it, ds, label, Math.max(1, Math.floor((W - labelW) / 40) - extra), sorting);
    model.warnings.forEach(warn);
    if (!model.columns.length) return;
    const cols = model.columns.length;
    const colW = (W - labelW) / (cols + extra);
    const hh = size * LINE + 9; // heading row
    const rh = size * LINE + 7; // body row
    let y = flow.y;
    const titleH = it.title ? 20 : 0;
    flow.ensure(titleH + hh + rh * Math.min(model.rows.length + 1, 3));
    if (it.title) {
      flow.add({ k: 'text', x: flow.left + 2, y: flow.y + 11, text: it.title, face: 'bold', size: 11, color: INK });
      flow.y += titleH;
    }
    const heading = (): void => {
      const top0 = flow.y;
      flow.add({ k: 'text', x: flow.left + 2, y: m.baseline(top0 + 3, size), text: fit(model.corner, labelW - 6, 'bold', size - 0.5, m), face: 'bold', size: size - 0.5, color: MUTED });
      const heads = [...model.columns.map(c => c.label), ...(it.rowTotals ? ['Total'] : [])];
      heads.forEach((h, i) => {
        const x = flow.left + labelW + i * colW;
        const text = fit(h, colW - 6, 'bold', size - 0.5, m);
        flow.add({ k: 'text', x: x + (colW - m.width(text, 'bold', size - 0.5)) / 2, y: m.baseline(top0 + 3, size), text, face: 'bold', size: size - 0.5, color: i >= cols ? INK : MUTED });
        const active = sorting?.column === i;
        flow.add({ k: 'rect', x, y: top0, w: colW, h: hh, hit: { ghost: true, cls: 'th', action: { type: 'sort', item: id, column: i }, tip: { rows: [{ label: active ? (sorting!.dir === 'asc' ? 'Click to sort descending' : 'Click to clear the sort') : 'Click to sort the rows by', value: h }] } } });
        if (active) {
          const ax = x + colW - 9;
          const cy = top0 + hh / 2 - 1;
          flow.add({ k: 'path', cmds: sorting!.dir === 'asc' ? [['M', ax, cy + 2], ['L', ax + 6, cy + 2], ['L', ax + 3, cy - 3], ['Z']] : [['M', ax, cy - 2], ['L', ax + 6, cy - 2], ['L', ax + 3, cy + 3], ['Z']], fill: '#1976d2' });
        }
      });
      flow.add({ k: 'line', x1: flow.left, y1: top0 + hh, x2: flow.left + W, y2: top0 + hh, color: '#9ca3af', lw: 1 });
      flow.y = top0 + hh;
    };
    heading();
    let first = true;
    for (const row of model.rows) {
      if (flow.full) break;
      if (flow.y + rh > flow.bottom + 0.01 && flow.y > flow.top + 0.01) {
        flow.newPage();
        heading();
      }
      y = flow.y;
      const bold = row.kind !== 'data';
      const tint = row.kind === 'data' ? undefined : '#f1f3f5';
      if (row.kind === 'total') flow.add({ k: 'line', x1: flow.left, y1: y, x2: flow.left + W, y2: y, color: '#6b7280', lw: 0.75 });
      else if (row.kind === 'group' && !first) flow.add({ k: 'line', x1: flow.left, y1: y, x2: flow.left + W, y2: y, color: '#e5e7eb', lw: 0.5 });
      first = false;
      if (tint) flow.add({ k: 'rect', x: flow.left, y, w: W, h: rh, fill: tint });
      const key = row.keys.length ? formatValue(row.keys[row.keys.length - 1]) : undefined;
      flow.add({ k: 'text', x: flow.left + 2 + (row.level ? 10 : 0), y: m.baseline(y + 2, size), text: fit(row.label, labelW - 8 - (row.level ? 10 : 0), bold ? 'bold' : 'regular', size, m), face: bold ? 'bold' : 'regular', size, color: INK });
      const cells = [...row.values, ...(it.rowTotals ? [row.total] : [])];
      cells.forEach((v, i) => {
        if (v === null) return;
        if (row.kind === 'group' && !it.subtotals) return;
        const x = flow.left + labelW + i * colW;
        const isTotal = i >= cols;
        const fill = row.kind === 'data' && !isTotal ? heatFill(it.heat, v, model.min, model.max) : isTotal && row.kind === 'data' ? '#f1f3f5' : undefined;
        const text = formatValue(v, fmt);
        const colLabel = isTotal ? 'Total' : model.columns[i].label;
        const hit = row.kind === 'data' && !isTotal
          ? {
              key, cls: 'cell',
              tip: { title: `${row.keys.map(k => formatValue(k)).join(' · ')} · ${colLabel}`, rows: [{ label: it.value, value: text, color: fill }, ...(it.drill ? [{ label: 'Click to open', value: it.drill.report }] : [])] },
              action: it.drill ? drillAction(it.drill, sc, { columns: [...it.rows, it.column, it.value], rows: [] }, [...row.keys, model.columns[i].key, v]) : undefined,
            }
          : undefined;
        if (fill) flow.add({ k: 'rect', x: x + 0.75, y: y + 0.75, w: colW - 1.5, h: rh - 1.5, fill, hit });
        const tw = m.width(text, bold || isTotal ? 'bold' : 'regular', size);
        flow.add({ k: 'text', x: it.heat === 'none' ? x + colW - tw - 8 : x + (colW - tw) / 2, y: m.baseline(y + 2, size), text, face: bold || isTotal ? 'bold' : 'regular', size, color: inkOn(fill), hit });
      });
      flow.y = y + rh;
    }
    flow.add({ k: 'line', x1: flow.left, y1: flow.y, x2: flow.left + W, y2: flow.y, color: '#d1d5db', lw: 0.5 });
    flow.y += 5;
    if (it.heat !== 'none' && model.max > model.min) {
      const ramp = heatRamp(it.heat, model.min, model.max);
      const lo = formatValue(model.min, fmt);
      const hi = formatValue(model.max, fmt);
      flow.ensure(16);
      const sw = 22;
      const x1 = flow.left + W - m.width(hi, 'regular', 7.5) - 4;
      ramp.forEach(([, fill], i) => flow.add({ k: 'rect', x: x1 - 6 - (5 - i) * sw, y: flow.y + 2, w: sw, h: 8, fill }));
      flow.add({ k: 'text', x: x1, y: flow.y + 9.5, text: hi, face: 'regular', size: 7.5, color: MUTED });
      flow.add({ k: 'text', x: x1 - 6 - 5 * sw - 4 - m.width(lo, 'regular', 7.5), y: flow.y + 9.5, text: lo, face: 'regular', size: 7.5, color: MUTED });
      flow.y += 14;
    }
    flow.y += 4;
  };

  /** Side by side: every column is laid out on pages of its own, then the pages are laid over each other. */
  const columns = (it: ColumnsItem, sc: Scope, flow: Flow, id: string, label: string, at?: string): void => {
    const cells = it.columns ?? [];
    if (!cells.length) return;
    const gap = it.gap ?? 12;
    const weights = cells.map(c => c.width ?? 1);
    const sum = weights.reduce((a, b) => a + b, 0) || 1;
    const room = flow.contentWidth - gap * (cells.length - 1);
    flow.ensure(60);
    const y0 = flow.y;
    const startPage = flow.pages.length;
    const first = env.page;
    let x = flow.left;
    const children = cells.map((cell, k) => {
      const w = (room * weights[k]) / sum;
      const pad = cell.card ? CARD_PAD : 0;
      const child = new Flow(flow.width, flow.height, x + pad, flow.top + pad, flow.bottom - pad, w - 2 * pad, input.maxPages ?? 2000, n => {
        env.page = first + n - 1;
      }, y0 + pad);
      const left = x;
      x += w + gap;
      layoutItems(cell.items ?? [], sc, child, `${id}.${k}`, label, at ? `${at}.columns.${k}.items` : undefined);
      return { cell, child, x: left, w, pad };
    });
    env.page = first;
    const pages = Math.max(...children.map(c => c.child.pages.length));
    // where the block ends on its last page: the lowest of the columns that got that far
    const end = Math.max(...children.filter(c => c.child.pages.length === pages).map(c => c.child.y + c.pad));
    for (let k = 0; k < pages; k++) {
      if (k > 0) flow.newPage();
      if (flow.full) break;
      const from = k === 0 ? y0 : flow.top;
      for (const c of children) {
        const own = c.child.pages.length;
        if (own <= k || !c.cell.card) continue;
        const to = k === pages - 1 ? end : k === own - 1 ? c.child.y + c.pad : flow.bottom;
        flow.add({ k: 'rect', x: c.x, y: from, w: c.w, h: Math.max(to - from, 8), fill: c.cell.background ?? '#ffffff', stroke: CARD_BORDER, lw: 0.75, r: c.cell.accent ? 0 : 4 });
        if (c.cell.accent) flow.add({ k: 'rect', x: c.x, y: from, w: c.w, h: 3, fill: c.cell.accent });
      }
      for (const c of children) {
        if (c.child.pages.length <= k) continue;
        for (const p of c.child.pages[k].prims) flow.add(p);
        const regions = c.child.pages[k].regions;
        if (regions && !flow.full) (flow.pages[flow.pages.length - 1].regions ??= []).push(...regions);
      }
    }
    flow.y = end + 4;
    void startPage;
  };

  const subreport = (it: SubreportItem, sc: Scope, flow: Flow, id: string, label: string): void => {
    const target = Object.values(sc.subs).find(r => r.report.name === it.report)?.report;
    if (!target) {
      warn(`${label}: sub-report ${it.report} is not available`);
      textItem({ type: 'text', text: `Sub-report "${it.report}" is not available.`, style: { size: 9, color: MUTED } }, sc, flow);
      return;
    }
    subCalls(it, target, sc.env, sc.datasets).forEach((call, i) => {
      if (flow.full) return;
      const run = sc.subs[subKey(it.report, call.params)];
      if (!run) {
        warn(`${label}: a run of sub-report ${it.report} is missing`);
        return;
      }
      if (i > 0) {
        if (it.pageBreak) flow.newPage();
        else flow.y += 10;
      }
      const inner: Scope = {
        report: run.report,
        env: Object.create(sc.env, { report: { value: run.report }, params: { value: run.params } }) as Env,
        datasets: run.datasets,
        subs: run.subs,
      };
      layoutItems(run.report.body, inner, flow, `${id}~${i}`, `${label}.${i + 1}`);
    });
  };

  const layoutItem = (it: BodyItem, sc: Scope, flow: Flow, id: number | string, label: string, at?: string): void => {
    switch (it.type) {
      case 'text': textItem(it, sc, flow); break;
      case 'table': table(it, sc, flow, id, `table ${label}`); break;
      case 'chart': chart(it, sc, flow, id, `chart ${label}`); break;
      case 'matrix': matrix(it, sc, flow, id, `matrix ${label}`); break;
      case 'image': imageItem(it, sc, flow); break;
      case 'columns': columns(it, sc, flow, String(id), label, at); break;
      case 'subreport': subreport(it, sc, flow, String(id), label); break;
      case 'rule':
        flow.ensure(6);
        flow.add({ k: 'line', x1: flow.left, y1: flow.y + 3, x2: flow.left + flow.contentWidth, y2: flow.y + 3, color: RULE, lw: 0.75 });
        flow.y += 6;
        break;
      case 'spacer': flow.y += it.height; break;
      case 'pageBreak': flow.newPage(); break;
    }
  };

  /** The items of a list one after the other; their ids continue `prefix`, so that clicks in the preview can name them. */
  const layoutItems = (items: BodyItem[], sc: Scope, flow: Flow, prefix: string, label: string, list?: string): void => {
    items.forEach((it, i) => {
      if (flow.full) return;
      if (list) flow.begin('body', i, list);
      layoutItem(it, sc, flow, prefix === '' ? i : `${prefix}/${i}`, label === '' ? String(i + 1) : `${label}.${i + 1}`, list ? `${list}.${i}` : undefined);
      if (list) flow.end();
    });
  };

  report.body.forEach((it, i) => {
    if (flow0.full) return;
    flow0.begin('body', i);
    layoutItem(it, root, flow0, i, String(i + 1), `body.${i}`);
    flow0.end();
  });
  if (flow0.full) warnings.push(`the report was cut at ${flow0.pages.length} pages`);

  // The header and footer need the number of pages.
  const count = flow0.pages.length;
  flow0.pages.forEach((page, i) => {
    const e = { ...env, page: i + 1, pageCount: count };
    drawBand('header', report.header, page, mg.top, e);
    drawBand('footer', report.footer, page, ph - mg.bottom - footerH, e);
  });
  return { pages: flow0.pages, warnings };
}
