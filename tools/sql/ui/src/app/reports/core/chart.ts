// Copyright (c) 2026 Philipp Orlov

// Charts as drawing primitives, so that the preview and the PDF show the same chart: bars (clustered, stacked or horizontal),
// lines, areas, pies and donuts, with data labels. Every mark carries what the preview needs to be interactive (a tooltip,
// a key that highlights it with the same category elsewhere, the legend's toggles); the PDF ignores that.
// Free of Angular imports on purpose.

import type { ChartItem } from './definition';
import { formatNumber, formatValue } from './format';
import { columnIndex, DataSet } from './scope';
import { Action, arcTo, Color, FontFace, PathCmd, Prim, Tip } from './primitives';

export const PALETTE: Color[] = ['#4e79a7', '#f28e2b', '#e15759', '#76b7b2', '#59a14f', '#edc948', '#b07aa1', '#ff9da7', '#9c755f', '#bab0ac'];

/** What the layout needs to know about text. */
export interface Measurer {
  width(text: string, face: FontFace, size: number): number;
}

export interface ChartSeries {
  label: string;
  color: Color;
  values: (number | null)[];
}

export interface ChartData {
  categories: string[];
  series: ChartSeries[];
}

export interface ChartBox {
  x: number;
  y: number;
  w: number;
  h: number;
}

export type ChartKind = 'bar' | 'line' | 'area' | 'pie';

export interface ChartSpec {
  kind: ChartKind;
  /** Bars: pile the series on each other. */
  stacked?: boolean;
  /** Bars: lie down, for long category names. */
  horizontal?: boolean;
  /** Pie: with a hole, and the total in it. */
  donut?: boolean;
  /** Write the values on the marks. */
  labels?: boolean;
}

export interface ChartOptions {
  title?: string;
  legend?: boolean;
  /** A number pattern for the axis, the labels and the tooltips. */
  format?: string;
}

/** Where the chart is in the report, and which of its series (slices, for a pie) the reader switched off. */
export interface ChartContext {
  item: number | string;
  hidden: number[];
  /** A click on the mark of category `c` (an index into `categories`) opens another report: what the click does, and its name for the tooltip. */
  drill?: { action(c: number): Action; label: string };
}

const INK = '#222222';
const MUTED = '#6b7280';
const GRID = '#e5e7eb';
const AXIS = '#9ca3af';
const BORDER = '#e0e0e0';

/** A round step size and an axis range that covers [min, max]. */
export function niceScale(min: number, max: number, ticks = 5): { min: number; max: number; step: number } {
  if (!(max > min)) {
    const pad = Math.abs(max) > 0 ? Math.abs(max) * 0.1 : 1;
    min -= pad;
    max += pad;
  }
  const raw = (max - min) / ticks;
  const mag = 10 ** Math.floor(Math.log10(raw));
  const norm = raw / mag;
  const step = (norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 5 ? 5 : 10) * mag;
  return { min: Math.floor(min / step + 1e-9) * step, max: Math.ceil(max / step - 1e-9) * step, step };
}

function ellipsize(text: string, width: number, face: FontFace, size: number, m: Measurer): string {
  if (m.width(text, face, size) <= width) return text;
  let s = text;
  while (s.length > 1 && m.width(s + '…', face, size) > width) s = s.slice(0, -1);
  return s.length > 1 || width > 8 ? s + '…' : '';
}

/** Compact numbers for labels on the marks: 1.2M, 340K. */
function short(v: number, fmt: string): string {
  const a = Math.abs(v);
  if (a >= 1e9) return formatNumber(v / 1e9, '0.#') + 'B';
  if (a >= 1e6) return formatNumber(v / 1e6, '0.#') + 'M';
  if (a >= 1e4) return formatNumber(v / 1e3, '0.#') + 'K';
  return formatNumber(v, fmt);
}

export function drawChart(spec: ChartSpec, data: ChartData, box: ChartBox, options: ChartOptions, m: Measurer, ctx: ChartContext = { item: 0, hidden: [] }): Prim[] {
  const prims: Prim[] = [];
  const fmt = options.format || '#,##0.##';
  let { x, y, w, h } = box;
  prims.push({ k: 'rect', x, y, w, h, fill: '#ffffff', stroke: BORDER, lw: 0.75 });
  x += 8;
  y += 6;
  w -= 16;
  h -= 12;
  if (options.title) {
    prims.push({ k: 'text', x, y: y + 11, text: options.title, face: 'bold', size: 11, color: INK });
    y += 20;
    h -= 20;
  }
  if (!data.categories.length || !data.series.length) {
    prims.push({ k: 'text', x: x + 4, y: y + 14, text: 'No data', face: 'regular', size: 9, color: MUTED });
    return prims;
  }
  if (spec.kind === 'pie') return prims.concat(pie(spec, data, { x, y, w, h }, options.legend !== false, fmt, m, ctx));

  // ---- legend: every series, the switched-off ones struck out; a click switches one --------------------------------
  const hidden = new Set(ctx.hidden);
  const visible = data.series.map((_, i) => i).filter(i => !hidden.has(i));
  const showLegend = options.legend ?? data.series.length > 1;
  if (showLegend) {
    let lx = x;
    let ly = y + h - 4;
    const rows: { i: number; label: string; width: number }[][] = [[]];
    let used = 0;
    data.series.forEach((s, i) => {
      const label = ellipsize(s.label, 150, 'regular', 8, m);
      const width = 18 + m.width(label, 'regular', 8) + 10;
      if (used + width > w && rows[rows.length - 1].length) {
        rows.push([]);
        used = 0;
      }
      rows[rows.length - 1].push({ i, label, width });
      used += width;
    });
    ly = y + h - 4 - (rows.length - 1) * 13;
    for (const row of rows) {
      lx = x;
      for (const { i, label, width } of row) {
        const off = hidden.has(i);
        const s = data.series[i];
        prims.push(off ? { k: 'rect', x: lx, y: ly - 7, w: 8, h: 8, stroke: s.color, lw: 1 } : { k: 'rect', x: lx, y: ly - 7, w: 8, h: 8, fill: s.color });
        prims.push({ k: 'text', x: lx + 12, y: ly, text: label, face: 'regular', size: 8, color: off ? '#9ca3af' : INK });
        if (off) prims.push({ k: 'line', x1: lx + 12, y1: ly - 2.5, x2: lx + 12 + m.width(label, 'regular', 8), y2: ly - 2.5, color: '#9ca3af', lw: 0.6 });
        prims.push({
          k: 'rect', x: lx - 2, y: ly - 10, w: width - 4, h: 13,
          hit: { ghost: true, cls: 'legend', key: undefined, action: { type: 'series', item: ctx.item, index: i }, tip: { rows: [{ label: off ? 'Click to show' : 'Click to hide', value: s.label, color: s.color }] } },
        });
        lx += width;
      }
      ly += 13;
    }
    h -= 4 + rows.length * 13;
  }

  const horizontal = spec.kind === 'bar' && !!spec.horizontal;
  const stacked = spec.kind === 'bar' && !!spec.stacked && visible.length > 1;

  // ---- the value range ------------------------------------------------------------------------------------------------
  const n = data.categories.length;
  const at = (s: number, c: number): number | null => {
    const v = data.series[s].values[c];
    return v === null || v === undefined || !Number.isFinite(v) ? null : v;
  };
  let lo = 0;
  let hi = 0;
  const all: number[] = [];
  for (let c = 0; c < n; c++) {
    let pos = 0;
    let neg = 0;
    for (const s of visible) {
      const v = at(s, c);
      if (v === null) continue;
      all.push(v);
      if (v >= 0) pos += v;
      else neg += v;
    }
    if (stacked) {
      hi = Math.max(hi, pos);
      lo = Math.min(lo, neg);
    }
  }
  if (!stacked) {
    hi = Math.max(0, ...all);
    lo = Math.min(0, ...all);
    if (spec.kind === 'line' && all.length) {
      lo = Math.min(...all);
      hi = Math.max(...all);
      if (lo > 0 && lo < hi * 0.5) lo = 0;
    }
  }
  const scale = niceScale(lo, hi);
  const ticks: number[] = [];
  for (let v = scale.min; v <= scale.max + scale.step * 1e-6; v += scale.step) ticks.push(Math.abs(v) < scale.step * 1e-6 ? 0 : v);
  const tickLabels = ticks.map(t => short(t, fmt));

  // ---- the plot area ----------------------------------------------------------------------------------------------------
  const catLabelFor = (c: number, room: number): string => ellipsize(data.categories[c], room, 'regular', 8, m);
  let px: number, py: number, pw: number, ph: number;
  if (horizontal) {
    const labelW = Math.min(Math.max(...data.categories.map(c => m.width(c, 'regular', 8))) + 8, w * 0.36);
    px = x + labelW;
    py = y + 2;
    pw = w - labelW - 14;
    ph = h - 18;
  } else {
    const left = Math.max(...tickLabels.map(l => m.width(l, 'regular', 8))) + 10;
    px = x + left;
    py = y + 8;
    pw = w - left - 6;
    ph = h - 26;
  }
  if (pw < 24 || ph < 24) return prims;

  const slotsAlong = horizontal ? ph : pw;
  const slot = slotsAlong / n;
  const valueOf = (v: number): number => (v - scale.min) / (scale.max - scale.min);
  // vertical: value -> y; horizontal: value -> x
  const vy = (v: number): number => py + ph - valueOf(v) * ph;
  const vx = (v: number): number => px + valueOf(v) * pw;

  ticks.forEach((t, i) => {
    if (horizontal) {
      const xx = vx(t);
      prims.push({ k: 'line', x1: xx, y1: py, x2: xx, y2: py + ph, color: GRID, lw: 0.6 });
      prims.push({ k: 'text', x: xx - m.width(tickLabels[i], 'regular', 8) / 2, y: py + ph + 11, text: tickLabels[i], face: 'regular', size: 8, color: MUTED });
    } else {
      const yy = vy(t);
      prims.push({ k: 'line', x1: px, y1: yy, x2: px + pw, y2: yy, color: GRID, lw: 0.6 });
      prims.push({ k: 'text', x: px - 5 - m.width(tickLabels[i], 'regular', 8), y: yy + 3, text: tickLabels[i], face: 'regular', size: 8, color: MUTED });
    }
  });
  if (horizontal) {
    prims.push({ k: 'line', x1: vx(0), y1: py, x2: vx(0), y2: py + ph, color: AXIS, lw: 0.8 });
  } else {
    prims.push({ k: 'line', x1: px, y1: vy(0), x2: px + pw, y2: vy(0), color: AXIS, lw: 0.8 });
  }

  // category labels, every k-th when they do not fit
  const widest = Math.max(...data.categories.map(c => m.width(c, 'regular', 8)));
  const every = horizontal ? Math.max(1, Math.ceil(10 / slot)) : Math.max(1, Math.ceil((widest + 4) / slot));
  data.categories.forEach((_, c) => {
    if (c % every) return;
    if (horizontal) {
      const label = catLabelFor(c, px - x - 8);
      prims.push({ k: 'text', x: px - 5 - m.width(label, 'regular', 8), y: py + slot * (c + 0.5) + 3, text: label, face: 'regular', size: 8, color: INK });
    } else {
      const label = catLabelFor(c, Math.max(slot * every - 4, 12));
      prims.push({ k: 'text', x: px + slot * (c + 0.5) - m.width(label, 'regular', 8) / 2, y: py + ph + 12, text: label, face: 'regular', size: 8, color: INK });
    }
  });

  const tipFor = (c: number): Tip => {
    const rows: Tip['rows'] = visible.map(s => ({ label: data.series[s].label, value: at(s, c) === null ? '—' : formatNumber(at(s, c)!, fmt), color: data.series[s].color }));
    if (stacked) rows.push({ label: 'Total', value: formatNumber(visible.reduce((a, s) => a + (at(s, c) ?? 0), 0), fmt), bold: true });
    if (ctx.drill) rows.push({ label: 'Click to open', value: ctx.drill.label });
    return { title: data.categories[c], rows };
  };

  // ---- the marks -----------------------------------------------------------------------------------------------------------
  const labelSize = 7;
  if (spec.kind === 'bar') {
    const groupSize = slot * 0.72;
    const barSize = stacked ? groupSize : groupSize / Math.max(visible.length, 1);
    for (let c = 0; c < n; c++) {
      let up = 0;
      let down = 0;
      visible.forEach((s, vi) => {
        const v = at(s, c);
        if (v === null) return;
        const color = data.series[s].color;
        let a = 0;
        let b = v;
        if (stacked) {
          if (v >= 0) {
            a = up;
            up += v;
            b = up;
          } else {
            a = down;
            down += v;
            b = down;
          }
        }
        const offset = (slot - groupSize) / 2 + (stacked ? 0 : vi * barSize);
        const hit = { key: data.categories[c], cls: 'bar', action: ctx.drill?.action(c) };
        let bx: number, by: number, bw: number, bh: number;
        if (horizontal) {
          const x0 = vx(a);
          const x1 = vx(b);
          bx = Math.min(x0, x1);
          bw = Math.max(Math.abs(x1 - x0), 0.4);
          by = py + slot * c + offset;
          bh = Math.max(barSize - 0.5, 0.5);
        } else {
          const y0 = vy(a);
          const y1 = vy(b);
          by = Math.min(y0, y1);
          bh = Math.max(Math.abs(y1 - y0), 0.4);
          bx = px + slot * c + offset;
          bw = Math.max(barSize - 0.5, 0.5);
        }
        prims.push({ k: 'rect', x: bx, y: by, w: bw, h: bh, fill: color, hit });
        if (spec.labels) {
          const text = short(v, fmt);
          const tw = m.width(text, 'regular', labelSize);
          if (horizontal) {
            if (stacked ? bw > tw + 4 : true) prims.push({ k: 'text', x: stacked ? bx + (bw - tw) / 2 : v >= 0 ? bx + bw + 3 : bx - tw - 3, y: by + bh / 2 + 2.5, text, face: 'regular', size: labelSize, color: stacked ? '#ffffff' : INK });
          } else if (stacked ? bh > 10 && bw > tw : bw > tw - 2) {
            prims.push({ k: 'text', x: bx + (bw - tw) / 2, y: stacked ? by + bh / 2 + 2.5 : v >= 0 ? by - 3 : by + bh + 8, text, face: 'regular', size: labelSize, color: stacked ? '#ffffff' : INK });
          }
        }
      });
    }
  } else {
    // line and area
    for (const s of visible) {
      const color = data.series[s].color;
      const pts: ({ x: number; y: number } | null)[] = [];
      for (let c = 0; c < n; c++) {
        const v = at(s, c);
        pts.push(v === null ? null : { x: px + slot * (c + 0.5), y: vy(v) });
      }
      let run: { x: number; y: number }[] = [];
      const flush = (): void => {
        if (run.length > 1) {
          if (spec.kind === 'area') {
            const base = vy(Math.max(scale.min, Math.min(0, scale.max)));
            const cmds: PathCmd[] = [['M', run[0].x, base]];
            for (const p of run) cmds.push(['L', p.x, p.y]);
            cmds.push(['L', run[run.length - 1].x, base], ['Z']);
            prims.push({ k: 'path', cmds, fill: color, fillOpacity: 0.22 });
          }
          prims.push({ k: 'path', cmds: run.map((p, i) => [i ? 'L' : 'M', p.x, p.y] as PathCmd), stroke: color, lw: 1.8 });
        }
        run = [];
      };
      for (const p of pts) {
        if (!p) flush();
        else run.push(p);
      }
      flush();
      const dots = n <= 40;
      pts.forEach((p, c) => {
        if (!p) return;
        if (dots) prims.push({ k: 'rect', x: p.x - 2, y: p.y - 2, w: 4, h: 4, fill: '#ffffff', stroke: color, lw: 1.2, hit: { key: data.categories[c], cls: 'point', action: ctx.drill?.action(c) } });
        if (spec.labels && n <= 16) {
          const text = short(at(s, c)!, fmt);
          prims.push({ k: 'text', x: p.x - m.width(text, 'regular', labelSize) / 2, y: p.y - 6, text, face: 'regular', size: labelSize, color: INK });
        }
      });
    }
  }

  // one ghost per category on top: the tooltip of the whole column or row, and the highlight key
  for (let c = 0; c < n; c++) {
    const key = data.categories[c];
    if (horizontal) prims.push({ k: 'rect', x: px, y: py + slot * c, w: pw, h: slot, hit: { ghost: true, cls: 'slot', key, tip: tipFor(c), action: ctx.drill?.action(c) } });
    else prims.push({ k: 'rect', x: px + slot * c, y: py, w: slot, h: ph, hit: { ghost: true, cls: 'slot', key, tip: tipFor(c), action: ctx.drill?.action(c) } });
  }
  return prims;
}

function pie(spec: ChartSpec, data: ChartData, box: ChartBox, legend: boolean, fmt: string, m: Measurer, ctx: ChartContext): Prim[] {
  const prims: Prim[] = [];
  const hidden = new Set(ctx.hidden);
  const first = data.series[0];
  const all = data.categories.map((c, i) => ({ idx: i, label: c, value: Math.max(0, first.values[i] ?? 0) })).filter(s => s.value > 0);
  // more than ten slices: the tail becomes "Other" (idx -1, not switchable)
  let slices = all;
  if (all.length > 11) slices = all.slice(0, 10).concat({ idx: -1, label: 'Other', value: all.slice(10).reduce((a, s) => a + s.value, 0) });
  const shown = slices.filter(s => !hidden.has(s.idx));
  const total = shown.reduce((a, s) => a + s.value, 0);
  const colorOf = (i: number): Color => PALETTE[i % PALETTE.length];
  if (!(total > 0)) {
    prims.push({ k: 'text', x: box.x + 4, y: box.y + 14, text: all.length ? 'Everything is switched off' : 'No data', face: 'regular', size: 9, color: MUTED });
  }
  const legendW = legend ? Math.min(box.w * 0.45, 200) : 0;
  const r = Math.max(10, Math.min(box.w - legendW - 12, box.h - 8) / 2);
  const cx = box.x + (box.w - legendW) / 2;
  const cy = box.y + box.h / 2;
  const inner = spec.donut ? r * 0.58 : 0;
  let angle = -Math.PI / 2;
  slices.forEach((s, i) => {
    if (hidden.has(s.idx)) return;
    const sweep = (s.value / total) * Math.PI * 2;
    const a1 = angle;
    const a2 = angle + sweep;
    const mid = (a1 + a2) / 2;
    const color = colorOf(i);
    const cmds: PathCmd[] = [];
    if (shown.length === 1) {
      cmds.push(['M', cx + r, cy]);
      arcTo(cmds, cx, cy, r, 0, Math.PI * 2);
      if (inner) {
        cmds.push(['M', cx + inner, cy]);
        arcTo(cmds, cx, cy, inner, 0, -Math.PI * 2);
      }
      cmds.push(['Z']);
    } else if (inner) {
      cmds.push(['M', cx + inner * Math.cos(a1), cy + inner * Math.sin(a1)], ['L', cx + r * Math.cos(a1), cy + r * Math.sin(a1)]);
      arcTo(cmds, cx, cy, r, a1, a2);
      cmds.push(['L', cx + inner * Math.cos(a2), cy + inner * Math.sin(a2)]);
      arcTo(cmds, cx, cy, inner, a2, a1);
      cmds.push(['Z']);
    } else {
      cmds.push(['M', cx, cy], ['L', cx + r * Math.cos(a1), cy + r * Math.sin(a1)]);
      arcTo(cmds, cx, cy, r, a1, a2);
      cmds.push(['Z']);
    }
    const pct = formatNumber((s.value / total) * 100, '0.#') + '%';
    prims.push({
      k: 'path', cmds, fill: color, stroke: '#ffffff', lw: 1,
      hit: {
        key: s.label, cls: 'slice', fx: { dx: Math.cos(mid) * 5, dy: Math.sin(mid) * 5 },
        action: ctx.drill && s.idx >= 0 ? ctx.drill.action(s.idx) : undefined,
        tip: { title: s.label, rows: [{ label: first.label || 'Value', value: formatNumber(s.value, fmt), color }, { label: 'Share', value: pct }, ...(ctx.drill && s.idx >= 0 ? [{ label: 'Click to open', value: ctx.drill.label }] : [])] },
      },
    });
    if (spec.labels !== false && sweep > 0.28) {
      const lr = inner ? (r + inner) / 2 : r * 0.64;
      const text = pct;
      prims.push({ k: 'text', x: cx + lr * Math.cos(mid) - m.width(text, 'bold', 8) / 2, y: cy + lr * Math.sin(mid) + 3, text, face: 'bold', size: 8, color: '#ffffff' });
    }
    angle = a2;
  });
  if (inner && total > 0) {
    const text = short(total, fmt);
    prims.push({ k: 'text', x: cx - m.width(text, 'bold', 13) / 2, y: cy + 3, text, face: 'bold', size: 13, color: INK });
    prims.push({ k: 'text', x: cx - m.width('Total', 'regular', 8) / 2, y: cy + 14, text: 'Total', face: 'regular', size: 8, color: MUTED });
  }
  if (legend) {
    const lx = box.x + box.w - legendW + 6;
    const rowH = 13;
    let ly = box.y + Math.max(8, (box.h - slices.length * rowH) / 2) + 8;
    const sum = slices.reduce((a, s) => a + s.value, 0);
    slices.forEach((s, i) => {
      const off = hidden.has(s.idx);
      const pct = formatNumber((s.value / (total || sum)) * 100, '0.#') + '%';
      const label = ellipsize(off ? s.label : `${s.label} (${pct})`, legendW - 24, 'regular', 8, m);
      prims.push(off ? { k: 'rect', x: lx, y: ly - 7, w: 8, h: 8, stroke: colorOf(i), lw: 1 } : { k: 'rect', x: lx, y: ly - 7, w: 8, h: 8, fill: colorOf(i) });
      prims.push({ k: 'text', x: lx + 12, y: ly, text: label, face: 'regular', size: 8, color: off ? '#9ca3af' : INK });
      if (off) prims.push({ k: 'line', x1: lx + 12, y1: ly - 2.5, x2: lx + 12 + m.width(label, 'regular', 8), y2: ly - 2.5, color: '#9ca3af', lw: 0.6 });
      prims.push({
        k: 'rect', x: lx - 2, y: ly - 10, w: legendW - 8, h: rowH,
        hit: { ghost: true, cls: 'legend', key: off ? undefined : s.label, action: s.idx >= 0 ? { type: 'series', item: ctx.item, index: s.idx } : undefined },
      });
      ly += rowH;
    });
  }
  return prims;
}


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
