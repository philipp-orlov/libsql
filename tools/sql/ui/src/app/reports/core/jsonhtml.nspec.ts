// Copyright (c) 2026 Philipp Orlov

import { jsonHtml } from './jsonhtml';

describe('jsonHtml', () => {
  it('colors keys, strings, numbers, literals and punctuation', () => {
    const h = jsonHtml('{"a": [1, -2.5e3, true, null, "x"]}');
    expect(h).toContain('<span class="key">"a"</span><span class="punct">:</span>');
    expect(h).toContain('<span class="num">1</span>');
    expect(h).toContain('<span class="num">-2.5e3</span>');
    expect(h).toContain('<span class="lit">true</span>');
    expect(h).toContain('<span class="lit">null</span>');
    expect(h).toContain('<span class="str">"x"</span>');
  });

  it('escapes markup and keeps the text unchanged otherwise', () => {
    const src = '{\n  "t": "<b>&\\"q\\"</b>"\n}';
    const h = jsonHtml(src);
    expect(h).not.toContain('<b>');
    expect(h.replace(/<[^>]+>/g, '').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&')).toBe(src);
  });

  it('survives an unterminated string', () => {
    expect(jsonHtml('{"a": "abc')).toContain('<span class="str">"abc</span>');
  });
});
