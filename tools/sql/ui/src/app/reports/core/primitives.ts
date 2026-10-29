// Copyright (c) 2026 Philipp Orlov

// What a laid-out report is made of: pages of drawing primitives in points, origin at the top left, y growing down.
// The on-screen preview (svg.ts) and the PDF writer (pdf.ts) draw the same primitives, so what you see is what you export.

/** `#rrggbb` (or `#rgb`). */
export type Color = string;
export type FontFace = 'regular' | 'bold';

/** What the on-screen preview shows when the pointer rests on a mark: a card with a title and rows. */
export interface Tip {
  title?: string;
  rows: { label: string; value: string; color?: Color; bold?: boolean }[];
}

/** What a click on a legend entry, a column heading or a group heading does. */
export type Action =
  | { type: 'series'; item: number; index: number }
  | { type: 'sort'; item: number; column: number }
  | { type: 'fold'; item: number; id: string };

/**
 * Interactivity of a mark. It is for the preview only: the PDF draws the marks and ignores this.
 * Marks with the same `key` (a category, a region) highlight together and a click selects the key.
 */
export interface Hit {
  tip?: Tip;
  key?: string;
  action?: Action;
  /** A pie slice moves this far out when the pointer is on it. */
  fx?: { dx: number; dy: number };
  /** Takes the pointer but is not drawn: the target of a whole column, a row, a legend entry. */
  ghost?: boolean;
  /** Styling hook in the preview: bar, slice, point, row, th, fold, legend... */
  cls?: string;
}

export interface TextPrim {
  k: 'text';
  x: number;
  /** The baseline. */
  y: number;
  text: string;
  face: FontFace;
  size: number;
  color: Color;
  hit?: Hit;
}

export interface RectPrim {
  k: 'rect';
  x: number;
  y: number;
  w: number;
  h: number;
  fill?: Color;
  fillOpacity?: number;
  stroke?: Color;
  lw?: number;
  hit?: Hit;
}

export interface LinePrim {
  k: 'line';
  x1: number;
  y1: number;
  x2: number;
  y2: number;
  color: Color;
  lw: number;
  dash?: number[];
}

/** M x y, L x y, C x1 y1 x2 y2 x y, Z. */
export type PathCmd = ['M', number, number] | ['L', number, number] | ['C', number, number, number, number, number, number] | ['Z'];

export interface PathPrim {
  k: 'path';
  cmds: PathCmd[];
  fill?: Color;
  fillOpacity?: number;
  stroke?: Color;
  lw?: number;
  hit?: Hit;
}

export type Prim = TextPrim | RectPrim | LinePrim | PathPrim;

/** Where an item of the report landed on a page, so that the preview can select it with a click. */
export interface Region {
  section: 'header' | 'body' | 'footer';
  index: number;
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface PageDoc {
  width: number;
  height: number;
  prims: Prim[];
  regions?: Region[];
}

/** `#rgb` or `#rrggbb` as [r, g, b] in 0..1. */
export function parseColor(color: Color): [number, number, number] {
  let h = color.startsWith('#') ? color.slice(1) : color;
  if (h.length === 3) h = [...h].map(c => c + c).join('');
  const n = parseInt(h, 16);
  if (h.length !== 6 || Number.isNaN(n)) return [0, 0, 0];
  return [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
}

/** The arc of a circle as cubic Bézier segments (at most a quarter turn each), appended to `cmds` after a move or line to its start. */
export function arcTo(cmds: PathCmd[], cx: number, cy: number, r: number, from: number, to: number): void {
  const turns = Math.max(1, Math.ceil(Math.abs(to - from) / (Math.PI / 2) - 1e-9));
  const step = (to - from) / turns;
  const k = (4 / 3) * Math.tan(step / 4);
  for (let i = 0; i < turns; i++) {
    const a = from + i * step;
    const b = a + step;
    cmds.push([
      'C',
      cx + r * (Math.cos(a) - k * Math.sin(a)), cy + r * (Math.sin(a) + k * Math.cos(a)),
      cx + r * (Math.cos(b) + k * Math.sin(b)), cy + r * (Math.sin(b) - k * Math.cos(b)),
      cx + r * Math.cos(b), cy + r * Math.sin(b),
    ]);
  }
}
