// Copyright (c) 2026 Philipp Orlov

// A PDF 1.4 writer for laid-out pages: text in an embedded, subsetted TrueType font (Type0 / CIDFontType2, Identity-H,
// with a ToUnicode map so the text can be selected and searched), rectangles, lines and paths. No libraries: streams are
// compressed with the platform's CompressionStream when there is one. Free of Angular imports on purpose.

import { FontFace, PageDoc, parseColor, Prim } from './primitives';
import { TrueTypeFont } from './ttf';

export interface PdfInfo {
  title?: string;
  author?: string;
  subject?: string;
  creator?: string;
  /** Defaults to now. */
  created?: Date;
}

const encoder = new TextEncoder();

function num(n: number): string {
  const s = n.toFixed(3);
  return s.includes('.') ? s.replace(/0+$/, '').replace(/\.$/, '') : s;
}

function hex(n: number, digits: number): string {
  return n.toString(16).toUpperCase().padStart(digits, '0');
}

/** A PDF text string: UTF-16BE with a byte order mark, in hex. */
function textString(s: string): string {
  let out = '<FEFF';
  for (let i = 0; i < s.length; i++) out += hex(s.charCodeAt(i), 4);
  return out + '>';
}

async function deflate(data: Uint8Array): Promise<Uint8Array | null> {
  if (typeof CompressionStream === 'undefined') return null;
  const stream = new CompressionStream('deflate');
  const writer = stream.writable.getWriter();
  void writer.write(data as BufferSource);
  void writer.close();
  return new Uint8Array(await new Response(stream.readable).arrayBuffer());
}

class Output {
  private chunks: Uint8Array[] = [];
  length = 0;
  push(bytes: Uint8Array | string): void {
    const b = typeof bytes === 'string' ? encoder.encode(bytes) : bytes;
    this.chunks.push(b);
    this.length += b.length;
  }
  bytes(): Uint8Array {
    const out = new Uint8Array(this.length);
    let at = 0;
    for (const c of this.chunks) {
      out.set(c, at);
      at += c.length;
    }
    return out;
  }
}

/** The glyphs of one font that the document uses, with the code points they came from. */
class FontUse {
  readonly glyphs = new Map<number, number>(); // glyph id -> a code point that maps to it
  constructor(readonly font: TrueTypeFont) {}
  encode(text: string): string {
    let out = '';
    for (const ch of text) {
      const cp = ch.codePointAt(0)!;
      const g = this.font.glyphId(cp);
      if (!this.glyphs.has(g)) this.glyphs.set(g, cp);
      out += hex(g, 4);
    }
    return out;
  }
}

function contentOf(page: PageDoc, uses: Record<FontFace, FontUse>, alphas: Map<number, string>): string {
  const out: string[] = [];
  const y = (v: number): string => num(page.height - v);
  const color = (c: string, op: 'rg' | 'RG'): string => parseColor(c).map(num).join(' ') + ' ' + op;
  let font: FontFace | null = null;
  let size = 0;
  const draw = (p: Prim): void => {
    if ('hit' in p && p.hit?.ghost) return; // takes the pointer in the preview and draws nothing
    out.push('q');
    if ((p.k === 'rect' || p.k === 'path') && p.fillOpacity !== undefined && p.fillOpacity < 1) {
      let name = alphas.get(p.fillOpacity);
      if (!name) alphas.set(p.fillOpacity, (name = `GS${alphas.size + 1}`));
      out.push(`/${name} gs`);
    }
    drawInner(p);
    out.push('Q');
  };
  const drawInner = (p: Prim): void => {
    switch (p.k) {
      case 'text': {
        if (!p.text) return;
        if (p.face !== font || p.size !== size) {
          font = p.face;
          size = p.size;
        }
        out.push(`BT ${color(p.color, 'rg')} /${p.face === 'bold' ? 'F2' : 'F1'} ${num(p.size)} Tf ${num(p.x)} ${y(p.y)} Td <${uses[p.face].encode(p.text)}> Tj ET`);
        return;
      }
      case 'rect': {
        const ops = [];
        if (p.fill) ops.push(color(p.fill, 'rg'));
        if (p.stroke) ops.push(color(p.stroke, 'RG'), `${num(p.lw ?? 1)} w`);
        ops.push(`${num(p.x)} ${y(p.y + p.h)} ${num(p.w)} ${num(p.h)} re`, p.fill && p.stroke ? 'B' : p.fill ? 'f' : 'S');
        out.push(ops.join(' '));
        return;
      }
      case 'line': {
        const dash = p.dash ? `[${p.dash.map(num).join(' ')}] 0 d` : '';
        out.push(`${color(p.color, 'RG')} ${num(p.lw)} w ${dash} ${num(p.x1)} ${y(p.y1)} m ${num(p.x2)} ${y(p.y2)} l S`);
        return;
      }
      case 'path': {
        const ops = [];
        if (p.fill) ops.push(color(p.fill, 'rg'));
        if (p.stroke) ops.push(color(p.stroke, 'RG'), `${num(p.lw ?? 1)} w`);
        for (const c of p.cmds) {
          if (c[0] === 'M') ops.push(`${num(c[1])} ${y(c[2])} m`);
          else if (c[0] === 'L') ops.push(`${num(c[1])} ${y(c[2])} l`);
          else if (c[0] === 'C') ops.push(`${num(c[1])} ${y(c[2])} ${num(c[3])} ${y(c[4])} ${num(c[5])} ${y(c[6])} c`);
          else ops.push('h');
        }
        ops.push(p.fill && p.stroke ? 'B' : p.fill ? 'f' : 'S');
        out.push(ops.join(' '));
      }
    }
  };
  for (const p of page.prims) draw(p);
  return out.join('\n');
}

/** ToUnicode CMap for the used glyphs. */
function toUnicode(use: FontUse): string {
  const entries = [...use.glyphs].filter(([g]) => g !== 0).sort((a, b) => a[0] - b[0]);
  const utf16 = (cp: number): string => {
    if (cp < 0x10000) return hex(cp, 4);
    const v = cp - 0x10000;
    return hex(0xd800 + (v >> 10), 4) + hex(0xdc00 + (v & 0x3ff), 4);
  };
  let s = '/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n' +
    '/CMapName /Adobe-Identity-UCS def /CMapType 2 def\n1 begincodespacerange <0000> <FFFF> endcodespacerange\n';
  for (let i = 0; i < entries.length; i += 100) {
    const batch = entries.slice(i, i + 100);
    s += `${batch.length} beginbfchar\n` + batch.map(([g, cp]) => `<${hex(g, 4)}> <${utf16(cp)}>`).join('\n') + '\nendbfchar\n';
  }
  return s + 'endcmap CMapName currentdict /CMap defineresource pop end end';
}

/**
 * The PDF of `pages`. `fonts` are the faces the primitives name; the glyphs the pages use are embedded.
 * Resolves to the file's bytes.
 */
export async function buildPdf(pages: PageDoc[], fonts: Record<FontFace, TrueTypeFont>, info: PdfInfo = {}): Promise<Uint8Array> {
  const uses: Record<FontFace, FontUse> = { regular: new FontUse(fonts.regular), bold: new FontUse(fonts.bold) };
  const alphas = new Map<number, string>();
  const contents = pages.map(p => contentOf(p, uses, alphas)); // also collects the glyphs

  const out = new Output();
  const offsets: number[] = [];
  const pending: number[] = [];
  let next = 1;
  const reserve = (): number => next++;
  const begin = (id: number): void => {
    offsets[id] = out.length;
    out.push(`${id} 0 obj\n`);
  };
  const object = (id: number, body: string): void => {
    begin(id);
    out.push(body + '\nendobj\n');
  };
  const stream = async (id: number, dict: string, data: Uint8Array | string, compress = true): Promise<void> => {
    const raw = typeof data === 'string' ? encoder.encode(data) : data;
    const packed = compress ? await deflate(raw) : null;
    begin(id);
    out.push(`<< ${dict}${packed ? ' /Filter /FlateDecode' : ''} /Length ${(packed ?? raw).length} >>\nstream\n`);
    out.push(packed ?? raw);
    out.push('\nendstream\nendobj\n');
  };

  out.push('%PDF-1.4\n%\xE2\xE3\xCF\xD3\n');
  const catalog = reserve();
  const pagesId = reserve();
  const infoId = reserve();
  const pageIds = pages.map(() => reserve());
  const contentIds = pages.map(() => reserve());
  const fontIds: Record<FontFace, number> = { regular: reserve(), bold: reserve() };
  const alphaIds = new Map<string, number>([...alphas.values()].map(name => [name, reserve()]));

  const created = info.created ?? new Date();
  const stamp = `D:${created.getUTCFullYear()}${String(created.getUTCMonth() + 1).padStart(2, '0')}${String(created.getUTCDate()).padStart(2, '0')}` +
    `${String(created.getUTCHours()).padStart(2, '0')}${String(created.getUTCMinutes()).padStart(2, '0')}${String(created.getUTCSeconds()).padStart(2, '0')}Z`;
  object(catalog, `<< /Type /Catalog /Pages ${pagesId} 0 R >>`);
  object(pagesId, `<< /Type /Pages /Kids [${pageIds.map(i => `${i} 0 R`).join(' ')}] /Count ${pages.length} >>`);
  object(infoId, `<< ${info.title ? `/Title ${textString(info.title)} ` : ''}${info.author ? `/Author ${textString(info.author)} ` : ''}` +
    `${info.subject ? `/Subject ${textString(info.subject)} ` : ''}/Creator ${textString(info.creator ?? 'libsql reports')} /Producer ${textString('libsql reports')} /CreationDate (${stamp}) >>`);
  const states = alphaIds.size ? ` /ExtGState << ${[...alphaIds].map(([name, id]) => `/${name} ${id} 0 R`).join(' ')} >>` : '';
  const resources = `<< /Font << /F1 ${fontIds.regular} 0 R /F2 ${fontIds.bold} 0 R >>${states} >>`;
  for (const [opacity, name] of alphas) object(alphaIds.get(name)!, `<< /Type /ExtGState /ca ${num(opacity)} /CA ${num(opacity)} >>`);
  pages.forEach((p, i) => {
    object(pageIds[i], `<< /Type /Page /Parent ${pagesId} 0 R /MediaBox [0 0 ${num(p.width)} ${num(p.height)}] /Resources ${resources} /Contents ${contentIds[i]} 0 R >>`);
  });
  for (let i = 0; i < pages.length; i++) await stream(contentIds[i], '', contents[i]);

  for (const face of ['regular', 'bold'] as const) {
    const use = uses[face];
    const font = use.font;
    const subset = font.subset(use.glyphs.keys());
    const tag = face === 'bold' ? 'BOLDAA' : 'REGUAA';
    const baseName = `${tag}+${font.postScriptName.replace(/[^A-Za-z0-9-]/g, '')}`;
    const cid = reserve();
    const descriptor = reserve();
    const file = reserve();
    const map = reserve();
    const scale = 1000 / font.unitsPerEm;
    const widths = [...use.glyphs.keys()].sort((a, b) => a - b).map(g => `${g} [${Math.round(font.advance(g) * scale)}]`).join(' ');
    object(fontIds[face], `<< /Type /Font /Subtype /Type0 /BaseFont /${baseName} /Encoding /Identity-H /DescendantFonts [${cid} 0 R] /ToUnicode ${map} 0 R >>`);
    object(cid, `<< /Type /Font /Subtype /CIDFontType2 /BaseFont /${baseName} /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> ` +
      `/FontDescriptor ${descriptor} 0 R /DW 1000 /W [${widths}] /CIDToGIDMap /Identity >>`);
    const [x0, y0, x1, y1] = font.bbox.map(v => Math.round(v * scale));
    object(descriptor, `<< /Type /FontDescriptor /FontName /${baseName} /Flags 32 /FontBBox [${x0} ${y0} ${x1} ${y1}] /ItalicAngle ${num(font.italicAngle)} ` +
      `/Ascent ${Math.round(font.ascent * scale)} /Descent ${Math.round(font.descent * scale)} /CapHeight ${Math.round(font.capHeight * scale)} ` +
      `/StemV ${face === 'bold' ? 140 : 80} /FontFile2 ${file} 0 R >>`);
    await stream(file, `/Length1 ${subset.data.length}`, subset.data);
    await stream(map, '', toUnicode(use));
  }
  void pending;

  const xref = out.length;
  const count = next;
  out.push(`xref\n0 ${count}\n0000000000 65535 f \n`);
  for (let i = 1; i < count; i++) out.push(`${String(offsets[i]).padStart(10, '0')} 00000 n \n`);
  out.push(`trailer\n<< /Size ${count} /Root ${catalog} 0 R /Info ${infoId} 0 R >>\nstartxref\n${xref}\n%%EOF\n`);
  return out.bytes();
}
