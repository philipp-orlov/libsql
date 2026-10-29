// Copyright (c) 2026 Philipp Orlov

// A TrueType reader for what reports need: character to glyph (cmap), advance widths (hmtx) for measuring text, the
// numbers a PDF font descriptor wants, and a subsetter that keeps only the glyphs a document uses. TrueType outlines
// (glyf) only; fonts with CFF outlines are refused. Free of Angular imports on purpose.

export class FontError extends Error {}

const align4 = (n: number): number => (n + 3) & ~3;

/** A glyph subset: the font file's bytes, still indexed by the original glyph ids. */
export interface FontSubset {
  data: Uint8Array;
}

interface Table {
  offset: number;
  length: number;
}

export class TrueTypeFont {
  readonly unitsPerEm: number;
  readonly ascent: number;
  readonly descent: number;
  readonly lineGap: number;
  readonly capHeight: number;
  readonly italicAngle: number;
  readonly bbox: [number, number, number, number];
  readonly numGlyphs: number;
  readonly postScriptName: string;

  private readonly view: DataView;
  private readonly tables = new Map<string, Table>();
  private readonly advances: Uint16Array;
  private readonly cmap = new Map<number, number>();
  private readonly longLoca: boolean;

  private constructor(readonly data: Uint8Array) {
    this.view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const tag = this.view.getUint32(0);
    if (tag !== 0x00010000 && tag !== 0x74727565 /* true */) {
      throw new FontError('not a TrueType font (CFF/OpenType outlines are not supported)');
    }
    const count = this.view.getUint16(4);
    for (let i = 0; i < count; i++) {
      const at = 12 + 16 * i;
      const name = String.fromCharCode(data[at], data[at + 1], data[at + 2], data[at + 3]);
      this.tables.set(name, { offset: this.view.getUint32(at + 8), length: this.view.getUint32(at + 12) });
    }
    for (const required of ['head', 'hhea', 'maxp', 'hmtx', 'cmap', 'loca', 'glyf']) {
      if (!this.tables.has(required)) throw new FontError(`font has no ${required} table`);
    }
    const head = this.table('head');
    this.unitsPerEm = this.view.getUint16(head + 18);
    this.bbox = [this.view.getInt16(head + 36), this.view.getInt16(head + 38), this.view.getInt16(head + 40), this.view.getInt16(head + 42)];
    this.longLoca = this.view.getInt16(head + 50) !== 0;
    const hhea = this.table('hhea');
    this.ascent = this.view.getInt16(hhea + 4);
    this.descent = this.view.getInt16(hhea + 6);
    this.lineGap = this.view.getInt16(hhea + 8);
    const metrics = this.view.getUint16(hhea + 34);
    this.numGlyphs = this.view.getUint16(this.table('maxp') + 4);

    const hmtx = this.table('hmtx');
    this.advances = new Uint16Array(this.numGlyphs);
    let last = 0;
    for (let g = 0; g < this.numGlyphs; g++) {
      if (g < metrics) last = this.view.getUint16(hmtx + 4 * g);
      this.advances[g] = last;
    }

    const os2 = this.tables.get('OS/2');
    this.capHeight = os2 && this.view.getUint16(os2.offset) >= 2 && os2.length >= 90 ? this.view.getInt16(os2.offset + 88) : Math.round(this.ascent * 0.7);
    const post = this.tables.get('post');
    this.italicAngle = post ? this.view.getInt32(post.offset + 4) / 65536 : 0;
    this.postScriptName = this.readName(6) || 'Font';
    this.readCmap();
  }

  static parse(data: Uint8Array): TrueTypeFont {
    return new TrueTypeFont(data);
  }

  private table(name: string): number {
    return this.tables.get(name)!.offset;
  }

  private readName(id: number): string {
    const t = this.tables.get('name');
    if (!t) return '';
    const count = this.view.getUint16(t.offset + 2);
    const strings = t.offset + this.view.getUint16(t.offset + 4);
    for (let i = 0; i < count; i++) {
      const r = t.offset + 6 + 12 * i;
      if (this.view.getUint16(r + 6) !== id) continue;
      const platform = this.view.getUint16(r);
      const length = this.view.getUint16(r + 8);
      const at = strings + this.view.getUint16(r + 10);
      let out = '';
      if (platform === 3 || platform === 0) for (let k = 0; k + 1 < length; k += 2) out += String.fromCharCode(this.view.getUint16(at + k));
      else for (let k = 0; k < length; k++) out += String.fromCharCode(this.view.getUint8(at + k));
      return out;
    }
    return '';
  }

  private readCmap(): void {
    const base = this.table('cmap');
    const count = this.view.getUint16(base + 2);
    let best = -1;
    let bestRank = -1;
    for (let i = 0; i < count; i++) {
      const platform = this.view.getUint16(base + 4 + 8 * i);
      const encoding = this.view.getUint16(base + 6 + 8 * i);
      const offset = this.view.getUint32(base + 8 + 8 * i);
      const format = this.view.getUint16(base + offset);
      // Prefer full Unicode (format 12), then the BMP.
      const rank = format === 12 && (platform === 3 || platform === 0) ? 3 : format === 4 && platform === 3 && encoding === 1 ? 2 : format === 4 && platform === 0 ? 1 : -1;
      if (rank > bestRank) {
        bestRank = rank;
        best = base + offset;
      }
    }
    if (best < 0) throw new FontError('font has no Unicode cmap');
    const format = this.view.getUint16(best);
    if (format === 12) {
      const groups = this.view.getUint32(best + 12);
      for (let g = 0; g < groups; g++) {
        const at = best + 16 + 12 * g;
        const start = this.view.getUint32(at);
        const end = this.view.getUint32(at + 4);
        const glyph = this.view.getUint32(at + 8);
        for (let c = start; c <= end && c - start < 0x10000; c++) this.cmap.set(c, glyph + (c - start));
      }
    } else {
      const segX2 = this.view.getUint16(best + 6);
      const ends = best + 14;
      const starts = ends + segX2 + 2;
      const deltas = starts + segX2;
      const ranges = deltas + segX2;
      for (let s = 0; s < segX2 / 2; s++) {
        const end = this.view.getUint16(ends + 2 * s);
        const start = this.view.getUint16(starts + 2 * s);
        const delta = this.view.getInt16(deltas + 2 * s);
        const rangeOffset = this.view.getUint16(ranges + 2 * s);
        for (let c = start; c <= end && c !== 0xffff; c++) {
          let glyph: number;
          if (rangeOffset === 0) glyph = (c + delta) & 0xffff;
          else {
            const at = ranges + 2 * s + rangeOffset + 2 * (c - start);
            glyph = this.view.getUint16(at);
            if (glyph !== 0) glyph = (glyph + delta) & 0xffff;
          }
          if (glyph !== 0) this.cmap.set(c, glyph);
        }
      }
    }
  }

  /** The glyph for a code point; 0 (.notdef) when the font has none. */
  glyphId(codePoint: number): number {
    return this.cmap.get(codePoint) ?? 0;
  }

  hasGlyph(codePoint: number): boolean {
    return this.cmap.has(codePoint);
  }

  /** Advance width of a glyph in font units. */
  advance(glyph: number): number {
    return glyph < this.numGlyphs ? this.advances[glyph] : 0;
  }

  /** The glyph ids of a string, one per code point. */
  glyphs(text: string): number[] {
    const out: number[] = [];
    for (const ch of text) out.push(this.glyphId(ch.codePointAt(0)!));
    return out;
  }

  /** The width of `text` at `size` points. */
  measure(text: string, size: number): number {
    let units = 0;
    for (const ch of text) units += this.advance(this.glyphId(ch.codePointAt(0)!));
    return (units * size) / this.unitsPerEm;
  }

  /** Where glyph `g`'s outline is in the glyf table: [start, end) relative to the table. */
  private glyphRange(g: number): [number, number] {
    const loca = this.table('loca');
    if (this.longLoca) return [this.view.getUint32(loca + 4 * g), this.view.getUint32(loca + 4 * g + 4)];
    return [this.view.getUint16(loca + 2 * g) * 2, this.view.getUint16(loca + 2 * g + 2) * 2];
  }

  /** The glyphs a composite glyph is made of. */
  private components(g: number): number[] {
    const [start, end] = this.glyphRange(g);
    if (end - start < 10) return [];
    let at = this.table('glyf') + start;
    if (this.view.getInt16(at) >= 0) return [];
    at += 10;
    const out: number[] = [];
    for (;;) {
      const flags = this.view.getUint16(at);
      out.push(this.view.getUint16(at + 2));
      at += 4 + (flags & 1 ? 4 : 2);
      if (flags & 8) at += 2;
      else if (flags & 0x40) at += 4;
      else if (flags & 0x80) at += 8;
      if (!(flags & 0x20)) break;
    }
    return out;
  }

  /**
   * A font file holding only the outlines of `used` (and the glyphs they are built from; glyph 0 always), still
   * indexed by the original glyph ids so the document can address glyphs by id. The tables a PDF needs are kept;
   * the rest (cmap, name, OpenType layout, kerning) is dropped.
   */
  subset(used: Iterable<number>): FontSubset {
    const keep = new Set<number>([0]);
    const queue = [...used];
    while (queue.length) {
      const g = queue.pop()!;
      if (g < 0 || g >= this.numGlyphs || keep.has(g)) continue;
      keep.add(g);
      queue.push(...this.components(g));
    }
    const glyf = this.table('glyf');
    const chunks: Uint8Array[] = [];
    const loca = new DataView(new ArrayBuffer(4 * (this.numGlyphs + 1)));
    let size = 0;
    for (let g = 0; g < this.numGlyphs; g++) {
      loca.setUint32(4 * g, size);
      if (!keep.has(g)) continue;
      const [start, end] = this.glyphRange(g);
      if (end > start) {
        const body = this.data.subarray(glyf + start, glyf + end);
        chunks.push(body);
        size += align4(body.length);
        if (body.length % 4) chunks.push(new Uint8Array(4 - (body.length % 4)));
      }
    }
    loca.setUint32(4 * this.numGlyphs, size);
    const glyfOut = new Uint8Array(size);
    let at = 0;
    for (const c of chunks) {
      glyfOut.set(c, at);
      at += c.length;
    }

    const out = new Map<string, Uint8Array>();
    const copy = (tag: string): void => {
      const t = this.tables.get(tag);
      if (t) out.set(tag, this.data.slice(t.offset, t.offset + t.length));
    };
    for (const tag of ['hhea', 'maxp', 'hmtx', 'cvt ', 'fpgm', 'prep']) copy(tag);
    const head = this.data.slice(this.table('head'), this.table('head') + this.tables.get('head')!.length);
    const hv = new DataView(head.buffer);
    hv.setUint32(8, 0); // checkSumAdjustment, set below
    hv.setInt16(50, 1); // long loca
    out.set('head', head);
    out.set('loca', new Uint8Array(loca.buffer));
    out.set('glyf', glyfOut);
    return { data: assemble(out) };
  }
}

function checksum(bytes: Uint8Array): number {
  let sum = 0;
  const n = align4(bytes.length);
  for (let i = 0; i < n; i += 4) {
    const w = ((bytes[i] ?? 0) << 24) | ((bytes[i + 1] ?? 0) << 16) | ((bytes[i + 2] ?? 0) << 8) | (bytes[i + 3] ?? 0);
    sum = (sum + (w >>> 0)) >>> 0;
  }
  return sum;
}

/** Writes a font file from tables (tags sorted, as the format wants), with checksums. */
function assemble(tables: Map<string, Uint8Array>): Uint8Array {
  const tags = [...tables.keys()].sort();
  const n = tags.length;
  let offset = 12 + 16 * n;
  const total = tags.reduce((sum, t) => sum + align4(tables.get(t)!.length), offset);
  const out = new Uint8Array(total);
  const v = new DataView(out.buffer);
  v.setUint32(0, 0x00010000);
  v.setUint16(4, n);
  const power = Math.floor(Math.log2(n));
  v.setUint16(6, 16 * 2 ** power);
  v.setUint16(8, power);
  v.setUint16(10, 16 * n - 16 * 2 ** power);
  tags.forEach((tag, i) => {
    const body = tables.get(tag)!;
    for (let k = 0; k < 4; k++) out[12 + 16 * i + k] = tag.charCodeAt(k);
    v.setUint32(12 + 16 * i + 4, checksum(body));
    v.setUint32(12 + 16 * i + 8, offset);
    v.setUint32(12 + 16 * i + 12, body.length);
    out.set(body, offset);
    offset += align4(body.length);
  });
  const adjust = (0xb1b0afba - checksum(out)) >>> 0;
  const head = tags.indexOf('head');
  v.setUint32(v.getUint32(12 + 16 * head + 8) + 8, adjust);
  return out;
}
