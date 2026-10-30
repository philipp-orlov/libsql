// Copyright (c) 2026 Philipp Orlov

// Rich text inside one text box: `[b]bold[/b]`, `[u]underline[/u]`, `[color=#e15759]...[/color]`, `[size=24]...[/size]`,
// `[bg=#fff3cd]...[/bg]` (short forms `[c=]`, `[s=]`), nested freely; `[/]` closes the innermost; a backslash in front of a
// bracket (`\[`) is a plain bracket; a line that starts with `- ` is a bullet. The `{{ expressions }}` of the template are
// replaced first, and what they give is never read as markup. Free of Angular imports on purpose.

import type { FontFace } from './primitives';

export interface RunStyle {
  bold?: boolean;
  underline?: boolean;
  color?: string;
  size?: number;
  bg?: string;
}

export interface Run {
  text: string;
  style: RunStyle;
}

export interface Paragraph {
  runs: Run[];
  bullet: boolean;
}

/** The text box's own look: what a run without markup gets. */
export interface BaseStyle {
  face: FontFace;
  size: number;
  color: string;
}

/** One piece of a line: a run's text (or part of it) with the style it is drawn in. */
export interface Fragment {
  text: string;
  face: FontFace;
  size: number;
  color: string;
  underline: boolean;
  bg?: string;
  width: number;
}

export interface RichLine {
  fragments: Fragment[];
  /** The width of the text, trailing spaces left out. */
  width: number;
  /** The biggest size on the line: it sets the height and the baseline. */
  size: number;
  /** Indent of a bulleted paragraph's lines. */
  indent: number;
  /** The first line of a bulleted paragraph. */
  bullet: boolean;
}

export const BULLET_INDENT = 12;

const TAG = /\\([[\]\\])|\[(\/?)(b|u|color|c|size|s|bg)(?:=([^\]]*))?\]|\[\/\]/g;
const COLOR = /^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})$/;

interface Token {
  kind: 'text' | 'open' | 'close';
  text?: string;
  tag?: string;
  arg?: string;
  /** `[/]` or the position in the source, for messages. */
  at?: number;
  bare?: boolean;
}

const canon = (tag: string): string => (tag === 'c' ? 'color' : tag === 's' ? 'size' : tag);

/** The pieces of `source` outside `{{ }}`; `fill` gets each `{{ }}` and gives its value, which is text and nothing else. */
function tokenize(source: string, fill: (expression: string) => string): Token[] {
  const out: Token[] = [];
  const push = (text: string): void => {
    if (text) out.push({ kind: 'text', text });
  };
  let last = 0;
  const markup = (chunk: string, base: number): void => {
    let from = 0;
    TAG.lastIndex = 0;
    for (let m = TAG.exec(chunk); m; m = TAG.exec(chunk)) {
      push(chunk.slice(from, m.index));
      from = m.index + m[0].length;
      if (m[1] !== undefined) push(m[1]);
      else if (m[0] === '[/]') out.push({ kind: 'close', bare: true, at: base + m.index });
      else if (m[2] === '/') out.push({ kind: 'close', tag: canon(m[3]), at: base + m.index });
      else out.push({ kind: 'open', tag: canon(m[3]), arg: m[4], at: base + m.index });
    }
    push(chunk.slice(from));
  };
  for (const m of source.matchAll(/\{\{([\s\S]*?)\}\}/g)) {
    markup(source.slice(last, m.index), last);
    push(fill(m[1]));
    last = m.index + m[0].length;
  }
  markup(source.slice(last), last);
  return out;
}

/** What is wrong with the markup of `text`, or null. Unclosed tags are fine (they end with the text); a close without an open, and bad colors or sizes, are not. */
export function checkRich(text: string): string | null {
  const stack: string[] = [];
  for (const t of tokenize(text, () => '')) {
    if (t.kind === 'open') {
      if (t.tag === 'color' || t.tag === 'bg') {
        if (!COLOR.test((t.arg ?? '').trim())) return `[${t.tag}=${t.arg ?? ''}] needs a color like #e15759`;
      } else if (t.tag === 'size') {
        const n = Number(t.arg);
        if (!(n >= 4 && n <= 120)) return `[size=${t.arg ?? ''}] needs a number of points between 4 and 120`;
      } else if (t.arg !== undefined) return `[${t.tag}] takes no value`;
      stack.push(t.tag!);
    } else if (t.kind === 'close') {
      const top = stack.pop();
      if (top === undefined) return t.bare ? '[/] closes nothing' : `[/${t.tag}] closes nothing`;
      if (!t.bare && top !== t.tag) return `[/${t.tag}] where [/${top}] was expected`;
    }
  }
  return null;
}

/** The paragraphs of a rich template, `fill` evaluating its `{{ }}` parts. Never throws: bad markup shows as the text it is. */
export function parseRich(source: string, fill: (expression: string) => string): Paragraph[] {
  const paragraphs: Paragraph[] = [{ runs: [], bullet: false }];
  const stack: { tag: string; style: RunStyle }[] = [];
  let style: RunStyle = {};
  const add = (text: string): void => {
    const lines = text.split(/\r?\n/);
    lines.forEach((line, i) => {
      if (i > 0) paragraphs.push({ runs: [], bullet: false });
      if (line) paragraphs[paragraphs.length - 1].runs.push({ text: line, style });
    });
  };
  for (const t of tokenize(source, fill)) {
    if (t.kind === 'text') add(t.text!);
    else if (t.kind === 'open') {
      const arg = (t.arg ?? '').trim();
      const next: RunStyle = { ...style };
      if (t.tag === 'b') next.bold = true;
      else if (t.tag === 'u') next.underline = true;
      else if (t.tag === 'color' && COLOR.test(arg)) next.color = arg;
      else if (t.tag === 'bg' && COLOR.test(arg)) next.bg = arg;
      else if (t.tag === 'size' && Number(arg) >= 4 && Number(arg) <= 120) next.size = Number(arg);
      stack.push({ tag: t.tag!, style });
      style = next;
    } else {
      const top = stack.pop();
      if (top) style = top.style;
    }
  }
  for (const p of paragraphs) {
    const first = p.runs[0];
    const m = first && /^(?:[-*•]) /.exec(first.text);
    if (m) {
      p.bullet = true;
      p.runs[0] = { ...first, text: first.text.slice(m[0].length) };
      if (!p.runs[0].text) p.runs.shift();
    }
  }
  return paragraphs;
}

/** The text without markup, for places that cannot show it (a menu, a sheet cell, a tooltip). */
export function plainText(paragraphs: Paragraph[]): string {
  return paragraphs.map(p => (p.bullet ? '• ' : '') + p.runs.map(r => r.text).join('')).join('\n');
}

export interface RichMeasurer {
  width(text: string, face: FontFace, size: number): number;
}

/** The effective look of a run on top of the text box's own. */
export function effective(base: BaseStyle, s: RunStyle): { face: FontFace; size: number; color: string; underline: boolean; bg?: string } {
  return { face: s.bold ? 'bold' : base.face, size: s.size ?? base.size, color: s.color ?? base.color, underline: !!s.underline, bg: s.bg };
}

/** Breaks the paragraphs into lines of at most `width`. Words longer than a line are broken. */
export function wrapRich(paragraphs: Paragraph[], width: number, base: BaseStyle, m: RichMeasurer): RichLine[] {
  const lines: RichLine[] = [];
  for (const p of paragraphs) {
    const indent = p.bullet ? BULLET_INDENT : 0;
    const avail = Math.max(width - indent, 8);
    let cur: Fragment[] = [];
    let first = true;
    const finish = (): void => {
      const last = cur[cur.length - 1];
      if (last && last.text.endsWith(' ')) {
        const text = last.text.trimEnd();
        last.text = text;
        last.width = m.width(text, last.face, last.size);
        if (!text) cur.pop();
      }
      lines.push({
        fragments: cur,
        width: cur.reduce((a, f) => a + f.width, 0),
        size: cur.length ? Math.max(...cur.map(f => f.size)) : base.size,
        indent,
        bullet: p.bullet && first,
      });
      first = false;
      cur = [];
    };
    let used = 0;
    const put = (text: string, e: ReturnType<typeof effective>, w: number): void => {
      const last = cur[cur.length - 1];
      if (last && last.face === e.face && last.size === e.size && last.color === e.color && last.underline === e.underline && last.bg === e.bg) {
        last.text += text;
        last.width += w;
      } else cur.push({ text, face: e.face, size: e.size, color: e.color, underline: e.underline, bg: e.bg, width: w });
      used += w;
    };
    for (const run of p.runs) {
      const e = effective(base, run.style);
      for (const word of run.text.split(/(?<= )/)) {
        if (!word) continue;
        const trimmed = m.width(word.trimEnd(), e.face, e.size);
        if (used + trimmed > avail && cur.length) {
          finish();
          used = 0;
        }
        if (trimmed > avail) {
          // one word wider than the line: cut it where it fits
          let rest = word;
          while (m.width(rest.trimEnd(), e.face, e.size) > avail && rest.length > 1) {
            let n = rest.length - 1;
            while (n > 1 && m.width(rest.slice(0, n), e.face, e.size) > avail) n--;
            put(rest.slice(0, n), e, m.width(rest.slice(0, n), e.face, e.size));
            finish();
            used = 0;
            rest = rest.slice(n);
          }
          put(rest, e, m.width(rest, e.face, e.size));
        } else put(word, e, m.width(word, e.face, e.size));
      }
    }
    finish();
  }
  return lines;
}
