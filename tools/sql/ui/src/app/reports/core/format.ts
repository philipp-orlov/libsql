// Copyright (c) 2026 Philipp Orlov

// Number and date formatting for report cells. A number format is a pattern such as `#,##0.00`, `0.0%` or `$#,##0`;
// a date format uses yyyy MM dd HH mm ss (`yyyy-MM-dd HH:mm`). Free of Angular imports on purpose.

export type Value = null | number | string | boolean;

/** `#,##0.00`-style patterns: digits `0` (always shown) and `#` (shown when needed), `,` groups thousands, `.` starts the
 * decimals, `%` multiplies by 100; any other text is kept as it is. */
export function formatNumber(value: number, pattern: string): string {
  if (!Number.isFinite(value)) return String(value);
  if (!pattern) return plainNumber(value);
  const m = /[#0,.]+/.exec(pattern);
  if (!m) return pattern;
  const prefix = pattern.slice(0, m.index);
  const suffix = pattern.slice(m.index + m[0].length);
  const body = m[0];
  const dot = body.indexOf('.');
  const intPart = dot < 0 ? body : body.slice(0, dot);
  const decPart = dot < 0 ? '' : body.slice(dot + 1);
  const grouping = intPart.includes(',');
  const minInt = (intPart.match(/0/g) ?? []).length;
  const maxDec = (decPart.match(/[0#]/g) ?? []).length;
  const minDec = (decPart.match(/0/g) ?? []).length;
  let v = suffix.includes('%') || prefix.includes('%') ? value * 100 : value;
  const negative = v < 0 && Math.round(Math.abs(v) * 10 ** maxDec) !== 0;
  v = Math.abs(v);
  let fixed = v.toFixed(maxDec);
  if (maxDec > minDec && fixed.includes('.')) {
    // trim optional (#) decimals but keep the mandatory (0) ones
    let end = fixed.length;
    while (end > fixed.indexOf('.') + 1 + minDec && fixed[end - 1] === '0') end--;
    fixed = fixed.slice(0, end);
    if (fixed.endsWith('.')) fixed = fixed.slice(0, -1);
  }
  let [whole, frac = ''] = fixed.split('.');
  whole = whole.padStart(minInt, '0');
  if (minInt === 0 && whole === '0' && frac) whole = '';
  if (grouping) whole = whole.replace(/\B(?=(\d{3})+(?!\d))/g, ',');
  return (negative ? '-' : '') + prefix + whole + (frac ? '.' + frac : '') + suffix;
}

function plainNumber(v: number): string {
  if (Number.isInteger(v)) return String(v);
  // up to 10 significant digits, no trailing zeros, no exponent for ordinary magnitudes
  const s = Number(v.toPrecision(10)).toString();
  return s.includes('e') ? v.toString() : s;
}

/** A JavaScript Date for what a database hands out: an ISO string, or a number of seconds, milliseconds or
 * microseconds since 1970 (told apart by size). null when it is neither. */
export function toDate(value: Value): Date | null {
  if (typeof value === 'number') {
    const a = Math.abs(value);
    const ms = a >= 1e14 ? value / 1000 : a >= 1e11 ? value : value * 1000;
    const d = new Date(ms);
    return Number.isNaN(d.getTime()) ? null : d;
  }
  if (typeof value === 'string' && value.trim()) {
    const text = value.trim();
    // `2026-10-29 13:05:00` is UTC for a database, as is a bare date
    const iso = /^\d{4}-\d{2}-\d{2}$/.test(text) ? text + 'T00:00:00Z' : /^\d{4}-\d{2}-\d{2}[ T]\d/.test(text) && !/(Z|[+-]\d{2}:?\d{2})$/.test(text) ? text.replace(' ', 'T') + 'Z' : text;
    const d = new Date(iso);
    return Number.isNaN(d.getTime()) ? null : d;
  }
  return null;
}

const pad = (n: number, w = 2): string => String(n).padStart(w, '0');

/** yyyy MM dd HH mm ss (UTC), with other text kept. */
export function formatDate(value: Value, pattern: string): string {
  const d = toDate(value);
  if (!d) return value === null ? '' : String(value);
  return pattern.replace(/yyyy|yy|MM|dd|HH|mm|ss/g, t => {
    switch (t) {
      case 'yyyy': return pad(d.getUTCFullYear(), 4);
      case 'yy': return pad(d.getUTCFullYear() % 100);
      case 'MM': return pad(d.getUTCMonth() + 1);
      case 'dd': return pad(d.getUTCDate());
      case 'HH': return pad(d.getUTCHours());
      case 'mm': return pad(d.getUTCMinutes());
      default: return pad(d.getUTCSeconds());
    }
  });
}

export function isDatePattern(pattern: string): boolean {
  return /yyyy|yy|MM|dd|HH|mm|ss/.test(pattern);
}

/** A cell as text, with the column's format when it has one. */
export function formatValue(value: Value, pattern = ''): string {
  if (value === null || value === undefined) return '';
  if (pattern && isDatePattern(pattern)) return formatDate(value, pattern);
  if (typeof value === 'number') return formatNumber(value, pattern);
  if (typeof value === 'boolean') return value ? 'true' : 'false';
  if (pattern && typeof value === 'string' && value.trim() !== '' && !Number.isNaN(Number(value))) return formatNumber(Number(value), pattern);
  return value;
}
