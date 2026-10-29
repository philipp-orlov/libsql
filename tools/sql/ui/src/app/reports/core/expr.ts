// Copyright (c) 2026 Philipp Orlov

// The small expression language of reports, used in `{{ ... }}` of text and headers and in computed columns:
//   numbers, 'strings' or "strings", true, false, null; names (a field of the current row, a report parameter, or
//   PageNumber, PageCount, ReportName, Now); + - * / %, comparisons, && || !, a ? b : c, parentheses;
//   functions: Sum Avg Min Max Count First Last (over the rows of the scope: the group, or the table),
//   Format(value, pattern) Upper Lower Trim Len Round(value, decimals) Abs Iif(c, a, b) Coalesce(...) Substring(s, start, len).
// Nothing is evaluated with eval or Function: expressions are parsed and walked. Free of Angular imports on purpose.

import { formatValue, Value } from './format';

export class ExprError extends Error {}

type Node =
  | { t: 'num'; v: number }
  | { t: 'str'; v: string }
  | { t: 'lit'; v: boolean | null }
  | { t: 'name'; v: string }
  | { t: 'un'; op: string; a: Node }
  | { t: 'bin'; op: string; a: Node; b: Node }
  | { t: 'cond'; c: Node; a: Node; b: Node }
  | { t: 'call'; f: string; args: Node[] };

interface Token {
  k: 'num' | 'str' | 'id' | 'op' | 'end';
  v: string;
}

function tokenize(src: string): Token[] {
  const out: Token[] = [];
  let i = 0;
  while (i < src.length) {
    const c = src[i];
    if (/\s/.test(c)) {
      i++;
    } else if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(src[i + 1] ?? ''))) {
      const m = /^(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?/.exec(src.slice(i))!;
      out.push({ k: 'num', v: m[0] });
      i += m[0].length;
    } else if (c === "'" || c === '"') {
      let j = i + 1;
      let s = '';
      while (j < src.length && src[j] !== c) {
        if (src[j] === '\\' && j + 1 < src.length) j++;
        s += src[j++];
      }
      if (j >= src.length) throw new ExprError('unterminated string');
      out.push({ k: 'str', v: s });
      i = j + 1;
    } else if (/[A-Za-z_]/.test(c) || c === '[') {
      if (c === '[') {
        const j = src.indexOf(']', i);
        if (j < 0) throw new ExprError('missing ]');
        out.push({ k: 'id', v: src.slice(i + 1, j) }); // [Field name with spaces]
        i = j + 1;
      } else {
        const m = /^[A-Za-z_][A-Za-z0-9_]*/.exec(src.slice(i))!;
        out.push({ k: 'id', v: m[0] });
        i += m[0].length;
      }
    } else {
      const two = src.slice(i, i + 2);
      if (['&&', '||', '==', '!=', '<=', '>='].includes(two)) {
        out.push({ k: 'op', v: two });
        i += 2;
      } else if ('+-*/%()<>!?:,='.includes(c)) {
        out.push({ k: 'op', v: c === '=' ? '==' : c });
        i++;
      } else {
        throw new ExprError(`unexpected '${c}'`);
      }
    }
  }
  out.push({ k: 'end', v: '' });
  return out;
}

const PRECEDENCE: Record<string, number> = { '||': 1, '&&': 2, '==': 3, '!=': 3, '<': 4, '>': 4, '<=': 4, '>=': 4, '+': 5, '-': 5, '*': 6, '/': 6, '%': 6 };

class Parser {
  private at = 0;
  constructor(private readonly tokens: Token[]) {}
  private peek(): Token {
    return this.tokens[this.at];
  }
  private take(): Token {
    return this.tokens[this.at++];
  }
  private accept(op: string): boolean {
    if (this.peek().k === 'op' && this.peek().v === op) {
      this.at++;
      return true;
    }
    return false;
  }
  parse(): Node {
    const n = this.ternary();
    if (this.peek().k !== 'end') throw new ExprError(`unexpected '${this.peek().v}'`);
    return n;
  }
  private ternary(): Node {
    const c = this.binary(1);
    if (!this.accept('?')) return c;
    const a = this.ternary();
    if (!this.accept(':')) throw new ExprError("expected ':'");
    return { t: 'cond', c, a, b: this.ternary() };
  }
  private binary(min: number): Node {
    let left = this.unary();
    for (;;) {
      const t = this.peek();
      const p = t.k === 'op' ? PRECEDENCE[t.v] : undefined;
      if (p === undefined || p < min) return left;
      this.take();
      left = { t: 'bin', op: t.v, a: left, b: this.binary(p + 1) };
    }
  }
  private unary(): Node {
    if (this.accept('-')) return { t: 'un', op: '-', a: this.unary() };
    if (this.accept('+')) return this.unary();
    if (this.accept('!')) return { t: 'un', op: '!', a: this.unary() };
    return this.primary();
  }
  private primary(): Node {
    const t = this.take();
    if (t.k === 'num') return { t: 'num', v: Number(t.v) };
    if (t.k === 'str') return { t: 'str', v: t.v };
    if (t.k === 'op' && t.v === '(') {
      const n = this.ternary();
      if (!this.accept(')')) throw new ExprError("expected ')'");
      return n;
    }
    if (t.k === 'id') {
      if (t.v === 'true') return { t: 'lit', v: true };
      if (t.v === 'false') return { t: 'lit', v: false };
      if (t.v === 'null') return { t: 'lit', v: null };
      if (this.accept('(')) {
        const args: Node[] = [];
        if (!this.accept(')')) {
          do args.push(this.ternary());
          while (this.accept(','));
          if (!this.accept(')')) throw new ExprError("expected ')'");
        }
        return { t: 'call', f: t.v, args };
      }
      return { t: 'name', v: t.v };
    }
    throw new ExprError(t.k === 'end' ? 'unexpected end' : `unexpected '${t.v}'`);
  }
}

const cache = new Map<string, Node>();

/** Parses `src`; throws ExprError. */
export function parseExpression(src: string): void {
  compile(src);
}

function compile(src: string): Node {
  let n = cache.get(src);
  if (!n) {
    n = new Parser(tokenize(src)).parse();
    if (cache.size > 500) cache.clear();
    cache.set(src, n);
  }
  return n;
}

/** What an expression sees. */
export interface ExprContext {
  /** A name: the current row's field, a parameter, or a built-in; undefined when there is none. */
  lookup(name: string, row: Value[] | null): Value | undefined;
  /** The rows aggregate functions run over. */
  rows: Value[][];
  /** The row being formatted, if any. */
  row: Value[] | null;
}

const AGGREGATES = new Set(['Sum', 'Avg', 'Min', 'Max', 'Count', 'First', 'Last']);

const num = (v: Value): number => (typeof v === 'number' ? v : typeof v === 'boolean' ? (v ? 1 : 0) : v === null ? 0 : Number(v));
const truthy = (v: Value): boolean => (typeof v === 'string' ? v !== '' : !!v);
const text = (v: Value): string => (v === null ? '' : formatValue(v));

function compare(a: Value, b: Value): number {
  if (a === null || b === null) return a === b ? 0 : a === null ? -1 : 1;
  if (typeof a === 'number' && typeof b === 'number') return a - b;
  return String(a).localeCompare(String(b));
}

function evalNode(n: Node, cx: ExprContext): Value {
  switch (n.t) {
    case 'num': return n.v;
    case 'str': return n.v;
    case 'lit': return n.v;
    case 'name': {
      const v = cx.lookup(n.v, cx.row);
      if (v === undefined) throw new ExprError(`unknown name ${n.v}`);
      return v;
    }
    case 'un': {
      const v = evalNode(n.a, cx);
      return n.op === '-' ? -num(v) : !truthy(v);
    }
    case 'cond': return truthy(evalNode(n.c, cx)) ? evalNode(n.a, cx) : evalNode(n.b, cx);
    case 'bin': {
      if (n.op === '&&') return truthy(evalNode(n.a, cx)) && truthy(evalNode(n.b, cx));
      if (n.op === '||') return truthy(evalNode(n.a, cx)) || truthy(evalNode(n.b, cx));
      const a = evalNode(n.a, cx);
      const b = evalNode(n.b, cx);
      switch (n.op) {
        case '+': return typeof a === 'string' || typeof b === 'string' ? text(a) + text(b) : num(a) + num(b);
        case '-': return num(a) - num(b);
        case '*': return num(a) * num(b);
        case '/': return num(b) === 0 ? null : num(a) / num(b);
        case '%': return num(b) === 0 ? null : num(a) % num(b);
        case '==': return compare(a, b) === 0;
        case '!=': return compare(a, b) !== 0;
        case '<': return compare(a, b) < 0;
        case '>': return compare(a, b) > 0;
        case '<=': return compare(a, b) <= 0;
        default: return compare(a, b) >= 0;
      }
    }
    case 'call': return call(n, cx);
  }
}

function call(n: Extract<Node, { t: 'call' }>, cx: ExprContext): Value {
  const f = n.f;
  if (AGGREGATES.has(f)) {
    if (f === 'Count' && n.args.length === 0) return cx.rows.length;
    if (n.args.length !== 1) throw new ExprError(`${f} takes one argument`);
    const values = cx.rows.map(r => evalNode(n.args[0], { ...cx, row: r }));
    const nums = values.filter((v): v is number => typeof v === 'number');
    switch (f) {
      case 'Sum': return nums.reduce((a, b) => a + b, 0);
      case 'Avg': return nums.length ? nums.reduce((a, b) => a + b, 0) / nums.length : null;
      case 'Min': return values.filter(v => v !== null).reduce<Value>((a, b) => (a === null || compare(b, a) < 0 ? b : a), null);
      case 'Max': return values.filter(v => v !== null).reduce<Value>((a, b) => (a === null || compare(b, a) > 0 ? b : a), null);
      case 'Count': return values.filter(v => v !== null).length;
      case 'First': return values.length ? values[0] : null;
      default: return values.length ? values[values.length - 1] : null;
    }
  }
  const a = n.args.map(x => evalNode(x, cx));
  const need = (count: number): void => {
    if (a.length < count) throw new ExprError(`${f} needs ${count} arguments`);
  };
  switch (f) {
    case 'Format': need(2); return formatValue(a[0], String(a[1]));
    case 'Upper': need(1); return text(a[0]).toUpperCase();
    case 'Lower': need(1); return text(a[0]).toLowerCase();
    case 'Trim': need(1); return text(a[0]).trim();
    case 'Len': need(1); return text(a[0]).length;
    case 'Round': need(1); { const d = a.length > 1 ? num(a[1]) : 0; return Math.round(num(a[0]) * 10 ** d) / 10 ** d; }
    case 'Abs': need(1); return Math.abs(num(a[0]));
    case 'Iif': need(3); return truthy(a[0]) ? a[1] : a[2];
    case 'Coalesce': return a.find(v => v !== null && v !== '') ?? null;
    case 'Substring': need(2); return text(a[0]).substr(num(a[1]), a.length > 2 ? num(a[2]) : undefined);
    default: throw new ExprError(`unknown function ${f}`);
  }
}

/** The value of `src`; throws ExprError. */
export function evaluate(src: string, cx: ExprContext): Value {
  return evalNode(compile(src), cx);
}

/** `text` with every `{{ expression }}` replaced by its value. A failing expression shows as `#Error`. */
export function interpolate(source: string, cx: ExprContext): string {
  return source.replace(/\{\{([\s\S]*?)\}\}/g, (_, expr: string) => {
    try {
      return text(evaluate(expr, cx));
    } catch {
      return '#Error';
    }
  });
}

/** The error in each `{{ }}` of `source`, or null when all parse (names are not checked here). */
export function checkTemplate(source: string): string | null {
  for (const m of source.matchAll(/\{\{([\s\S]*?)\}\}/g)) {
    try {
      parseExpression(m[1]);
    } catch (e) {
      return (e as Error).message;
    }
  }
  return null;
}
