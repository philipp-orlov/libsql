// Copyright (c) 2026 Philipp Orlov

// JSON text as HTML with a span of a class on every token: `key`, `str`, `num`, `lit` (true, false, null) and `punct`.
// It is lenient on purpose: half-typed JSON is colored as far as it goes. Free of Angular imports.

const TOKEN = /("(?:\\.|[^"\\\n])*"?)(\s*:)?|-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|\b(?:true|false|null)\b|[{}[\],:]/g;

const escape = (s: string): string => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

export function jsonHtml(text: string): string {
  let out = '';
  let last = 0;
  for (const m of text.matchAll(TOKEN)) {
    out += escape(text.slice(last, m.index));
    last = m.index + m[0].length;
    const t = m[0];
    if (m[1] !== undefined) {
      const key = m[2] !== undefined;
      out += `<span class="${key ? 'key' : 'str'}">${escape(m[1])}</span>`;
      if (key) out += `<span class="punct">${escape(m[2]!)}</span>`;
    } else if (/^[{}[\],:]$/.test(t)) out += `<span class="punct">${t}</span>`;
    else if (/^[tfn]/.test(t)) out += `<span class="lit">${t}</span>`;
    else out += `<span class="num">${t}</span>`;
  }
  return out + escape(text.slice(last));
}
