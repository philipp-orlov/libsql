// Copyright (c) 2026 Philipp Orlov

import * as zlib from 'node:zlib';
import { buildPdf } from './pdf';
import { arcTo, PageDoc, PathCmd } from './primitives';
import { testFonts } from './test-fonts';
import { FontError, TrueTypeFont } from './ttf';

const fonts = testFonts();
const latin = (b: Uint8Array): string => Buffer.from(b).toString('latin1');

describe('TrueTypeFont', () => {
  it('reads the metrics', () => {
    const f = fonts.regular;
    expect(f.unitsPerEm).toBe(2048);
    expect(f.postScriptName).toBe('LiberationSans');
    expect(f.numGlyphs).toBeGreaterThan(2000);
    expect(f.measure('Hello', 10)).toBeCloseTo(22.79, 1);
    expect(f.measure('', 10)).toBe(0);
    expect(fonts.bold.measure('Hello', 10)).toBeGreaterThan(f.measure('Hello', 10));
  });
  it('maps Latin, Greek, Cyrillic and astral code points; missing ones are glyph 0', () => {
    const f = fonts.regular;
    for (const ch of ['A', 'é', 'Ж', 'λ', '€']) expect(f.glyphId(ch.codePointAt(0)!)).toBeGreaterThan(0);
    expect(f.glyphId(0x4e2d)).toBe(0); // a CJK character: not in this font
    expect(f.hasGlyph(0x4e2d)).toBe(false);
    expect(f.glyphs('AB').length).toBe(2);
  });
  it('subsets: only the used glyphs keep their outlines, and the file checks out', () => {
    const f = fonts.regular;
    const used = f.glyphs('Hello wörld');
    const sub = f.subset(used);
    expect(sub.data.length).toBeLessThan(f.data.length / 4);
    // the table directory is sorted and every table checksum is right; the whole file sums to 0xB1B0AFBA
    const v = new DataView(sub.data.buffer, sub.data.byteOffset, sub.data.byteLength);
    let sum = 0;
    for (let i = 0; i < sub.data.length; i += 4) sum = (sum + v.getUint32(i)) >>> 0;
    expect(sum).toBe(0xb1b0afba);
    const tags: string[] = [];
    for (let i = 0; i < v.getUint16(4); i++) tags.push(String.fromCharCode(...sub.data.subarray(12 + 16 * i, 16 + 16 * i)));
    expect(tags).toEqual([...tags].sort());
    expect(tags).toContain('glyf');
    expect(tags).not.toContain('cmap');
    // an unused glyph has no outline: loca[g] == loca[g+1]
    const at = (tag: string): number => v.getUint32(12 + 16 * tags.indexOf(tag) + 8);
    const loca = at('loca');
    const unused = f.glyphId('Z'.codePointAt(0)!);
    expect(v.getUint32(loca + 4 * unused)).toBe(v.getUint32(loca + 4 * unused + 4));
    const h = f.glyphId('H'.codePointAt(0)!);
    expect(v.getUint32(loca + 4 * h + 4)).toBeGreaterThan(v.getUint32(loca + 4 * h));
  });
  it('keeps the parts of a composite glyph', () => {
    const f = fonts.regular;
    // é is built from e and an accent
    const sub = f.subset([f.glyphId('é'.codePointAt(0)!)]);
    const e = sub.data.length;
    const plain = f.subset([0]).data.length;
    expect(e).toBeGreaterThan(plain);
  });
  it('refuses what is not TrueType', () => {
    expect(() => TrueTypeFont.parse(new Uint8Array(100))).toThrowError(FontError);
    const truncated = fonts.regular.data.slice(0, 200);
    expect(() => TrueTypeFont.parse(truncated)).toThrow();
    const otf = fonts.regular.data.slice();
    otf.set([0x4f, 0x54, 0x54, 0x4f]); // 'OTTO': CFF outlines
    expect(() => TrueTypeFont.parse(otf)).toThrowError(/CFF/);
  });
});

function parsePdf(bytes: Uint8Array): { text: string; objects: Map<number, { dict: string; stream?: Uint8Array }>; startxref: number } {
  const text = latin(bytes);
  const startxref = Number(/startxref\n(\d+)\n%%EOF\n$/.exec(text)![1]);
  expect(text.startsWith('%PDF-1.4')).toBe(true);
  expect(text.slice(startxref, startxref + 4)).toBe('xref');
  const lines = text.slice(startxref).split('\n');
  const [, count] = lines[1].split(' ').map(Number);
  const objects = new Map<number, { dict: string; stream?: Uint8Array }>();
  for (let i = 1; i < count; i++) {
    const offset = Number(lines[2 + i].slice(0, 10));
    expect(text.slice(offset, offset + `${i} 0 obj`.length)).toBe(`${i} 0 obj`);
    const end = text.indexOf('endobj', offset);
    const body = text.slice(offset + `${i} 0 obj\n`.length, end);
    const s = body.indexOf('stream\n');
    if (s < 0) objects.set(i, { dict: body.trim() });
    else {
      const length = Number(/\/Length (\d+)/.exec(body)![1]);
      const from = offset + `${i} 0 obj\n`.length + s + 7;
      objects.set(i, { dict: body.slice(0, s).trim(), stream: bytes.subarray(from, from + length) });
    }
  }
  return { text, objects, startxref };
}

describe('buildPdf', () => {
  const pie: PathCmd[] = [['M', 100, 100], ['L', 150, 100]];
  arcTo(pie, 100, 100, 50, 0, 2);
  pie.push(['Z']);
  const page: PageDoc = {
    width: 300,
    height: 200,
    prims: [
      { k: 'text', x: 10, y: 20, text: 'Hello Привет', face: 'regular', size: 12, color: '#112233' },
      { k: 'text', x: 10, y: 40, text: 'Bold', face: 'bold', size: 12, color: '#000' },
      { k: 'rect', x: 5, y: 5, w: 50, h: 20, fill: '#eee', stroke: '#333', lw: 1 },
      { k: 'line', x1: 0, y1: 0, x2: 10, y2: 10, color: '#f00', lw: 1, dash: [2, 1] },
      { k: 'path', cmds: pie, fill: '#0a0' },
    ],
  };

  it('writes a well-formed file: header, xref offsets, trailer', async () => {
    const pdf = await buildPdf([page, page], fonts, { title: 'T — тест', created: new Date(Date.UTC(2026, 0, 2, 3, 4, 5)) });
    const { text, objects } = parsePdf(pdf);
    expect(text).toMatch(/\/Type \/Pages \/Kids \[\d+ 0 R \d+ 0 R\] \/Count 2/);
    expect(text).toContain('/CreationDate (D:20260102030405Z)');
    expect(text).toContain('/Title <FEFF');
    expect(objects.size).toBeGreaterThan(10);
  });

  it('draws with the page origin at the bottom left, and keeps graphics state apart', async () => {
    const { objects } = parsePdf(await buildPdf([page], fonts));
    const contents = [...objects.values()].filter(o => o.stream && !/FontFile|Length1/.test(o.dict) && o.dict.includes('FlateDecode'));
    const streams = contents.map(o => zlib.inflateSync(o.stream!).toString('latin1'));
    const content = streams.find(s => s.includes(' Tf '))!;
    expect(content).toContain('/F1 12 Tf 10 180 Td'); // y 20 from the top of a 200 high page
    expect(content).toContain('/F2 12 Tf 10 160 Td');
    expect(content).toContain('5 175 50 20 re B');
    expect(content).toContain('[2 1] 0 d');
    expect(content.split('\nq\n').length + (content.startsWith('q\n') ? 1 : 0)).toBeGreaterThan(4);
    expect(content.match(/^q$/gm)!.length).toBe(content.match(/^Q$/gm)!.length);
  });

  it('embeds a subset of each font with widths and a ToUnicode map', async () => {
    const { objects, text } = parsePdf(await buildPdf([page], fonts));
    expect(text).toContain('/Subtype /Type0');
    expect(text).toContain('/Subtype /CIDFontType2');
    expect(text).toContain('/Encoding /Identity-H');
    const maps = [...objects.values()].filter(o => o.stream && zlib.inflateSync(o.stream).toString('latin1').includes('begincmap'));
    expect(maps.length).toBe(2);
    const regularMap = zlib.inflateSync(maps[0].stream!).toString('latin1');
    const gP = fonts.regular.glyphId('П'.codePointAt(0)!).toString(16).toUpperCase().padStart(4, '0');
    expect(regularMap).toContain(`<${gP}> <041F>`); // glyph of П maps back to U+041F
    const files = [...objects.values()].filter(o => /Length1/.test(o.dict));
    expect(files.length).toBe(2);
    const regular = zlib.inflateSync(files[0].stream!);
    expect(regular.length).toBeLessThan(fonts.regular.data.length / 5);
  });

  it('is much smaller than the fonts it draws with', async () => {
    const pdf = await buildPdf([page], fonts);
    expect(pdf.length).toBeLessThan(40_000);
  });
});
