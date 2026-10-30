// Copyright (c) 2026 Philipp Orlov

// Pictures in a report are `data:` URIs. This reads what layout needs (the kind and the size in pixels) from the header of
// PNG, JPEG, GIF, WebP and SVG without decoding them, and decodes PNG to pixels for the PDF (JPEG goes in as it is; the
// others are drawn to a canvas by whoever has one). Free of Angular imports on purpose.

import { inflate } from './compress';

export type ImageKind = 'png' | 'jpeg' | 'gif' | 'webp' | 'svg';

export interface ImageInfo {
  kind: ImageKind;
  mime: string;
  bytes: Uint8Array;
  /** In pixels (an SVG: its own units, which are CSS pixels). */
  width: number;
  height: number;
}

const encoder = new TextEncoder();

/** The bytes and the media type of a `data:` URI, or null when it is not one. */
export function decodeDataUri(src: string): { mime: string; bytes: Uint8Array } | null {
  const m = /^data:([^;,]*)((?:;[^;,]*)*),([\s\S]*)$/i.exec(src.trim());
  if (!m) return null;
  const mime = (m[1] || 'text/plain').toLowerCase();
  try {
    if (/;base64/i.test(m[2])) {
      const bin = atob(m[3].replace(/\s+/g, ''));
      const bytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
      return { mime, bytes };
    }
    return { mime, bytes: encoder.encode(decodeURIComponent(m[3])) };
  } catch {
    return null;
  }
}

const u16 = (b: Uint8Array, i: number): number => (b[i] << 8) | b[i + 1];
const u32 = (b: Uint8Array, i: number): number => ((b[i] << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3]) >>> 0;

function jpegSize(b: Uint8Array): [number, number] | null {
  let i = 2;
  while (i + 9 < b.length) {
    if (b[i] !== 0xff) {
      i++;
      continue;
    }
    const marker = b[i + 1];
    if (marker === 0xff) {
      i++;
      continue;
    }
    if (marker === 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker === 0x01) {
      i += 2;
      continue;
    }
    const len = u16(b, i + 2);
    if (marker >= 0xc0 && marker <= 0xcf && marker !== 0xc4 && marker !== 0xc8 && marker !== 0xcc) return [u16(b, i + 7), u16(b, i + 5)];
    i += 2 + len;
  }
  return null;
}

function webpSize(b: Uint8Array): [number, number] | null {
  if (b.length < 30) return null;
  const tag = String.fromCharCode(b[12], b[13], b[14], b[15]);
  if (tag === 'VP8X') return [1 + (b[24] | (b[25] << 8) | (b[26] << 16)), 1 + (b[27] | (b[28] << 8) | (b[29] << 16))];
  if (tag === 'VP8L') {
    const bits = b[21] | (b[22] << 8) | (b[23] << 16) | (b[24] << 24);
    return [1 + (bits & 0x3fff), 1 + ((bits >>> 14) & 0x3fff)];
  }
  if (tag === 'VP8 ') return [(b[26] | (b[27] << 8)) & 0x3fff, (b[28] | (b[29] << 8)) & 0x3fff];
  return null;
}

function svgSize(text: string): [number, number] | null {
  const root = /<svg\b[^>]*>/i.exec(text)?.[0];
  if (!root) return null;
  const attr = (name: string): string | undefined => new RegExp(`\\s${name}\\s*=\\s*["']([^"']*)["']`, 'i').exec(root)?.[1];
  const unit = (v: string | undefined): number | null => {
    const m = v && /^\s*([\d.]+)\s*(px|pt|pc|mm|cm|in|)\s*$/i.exec(v);
    if (!m) return null;
    const f = { px: 1, '': 1, pt: 4 / 3, pc: 16, mm: 96 / 25.4, cm: 96 / 2.54, in: 96 }[m[2].toLowerCase()] ?? 1;
    return Number(m[1]) * f;
  };
  const w = unit(attr('width'));
  const h = unit(attr('height'));
  const box = attr('viewBox')?.trim().split(/[\s,]+/).map(Number);
  const vb = box && box.length === 4 && box[2] > 0 && box[3] > 0 ? [box[2], box[3]] : null;
  if (w && h) return [w, h];
  if (vb) return w ? [w, (w * vb[1]) / vb[0]] : h ? [(h * vb[0]) / vb[1], h] : [vb[0], vb[1]];
  return null;
}

const cache = new Map<string, ImageInfo | null>();

/** What a `src` is, or null when it is not a picture this code can place (the report then says so). */
export function imageInfo(src: string): ImageInfo | null {
  if (cache.has(src)) return cache.get(src)!;
  let info: ImageInfo | null = null;
  const d = decodeDataUri(src);
  if (d) {
    const b = d.bytes;
    let kind: ImageKind | null = null;
    let size: [number, number] | null = null;
    if (b.length > 24 && u32(b, 0) === 0x89504e47 && u32(b, 4) === 0x0d0a1a0a) {
      kind = 'png';
      size = [u32(b, 16), u32(b, 20)];
    } else if (b.length > 4 && b[0] === 0xff && b[1] === 0xd8) {
      kind = 'jpeg';
      size = jpegSize(b);
    } else if (b.length > 10 && String.fromCharCode(b[0], b[1], b[2]) === 'GIF') {
      kind = 'gif';
      size = [b[6] | (b[7] << 8), b[8] | (b[9] << 8)];
    } else if (b.length > 12 && String.fromCharCode(b[0], b[1], b[2], b[3]) === 'RIFF' && String.fromCharCode(b[8], b[9], b[10], b[11]) === 'WEBP') {
      kind = 'webp';
      size = webpSize(b);
    } else if (/svg/.test(d.mime) || /<svg\b/i.test(new TextDecoder().decode(b.subarray(0, 2048)))) {
      kind = 'svg';
      size = svgSize(new TextDecoder().decode(b));
    }
    if (kind && size && size[0] > 0 && size[1] > 0) info = { kind, mime: kind === 'svg' ? 'image/svg+xml' : `image/${kind}`, bytes: b, width: size[0], height: size[1] };
  }
  if (cache.size > 200) cache.clear();
  cache.set(src, info);
  return info;
}

/** Pixels as the PDF and Word writers want them: RGB, and a separate alpha plane when the picture has transparency. */
export interface Raster {
  width: number;
  height: number;
  rgb: Uint8Array;
  alpha?: Uint8Array;
}

const CHANNELS: Record<number, number> = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 };

/** Decodes a PNG (any bit depth and color type, not interlaced). Throws with the reason when it cannot. */
export async function decodePng(b: Uint8Array): Promise<Raster> {
  let w = 0;
  let h = 0;
  let depth = 8;
  let type = 2;
  let interlace = 0;
  let palette: Uint8Array | null = null;
  let trns: Uint8Array | null = null;
  const parts: Uint8Array[] = [];
  for (let i = 8; i + 8 <= b.length; ) {
    const len = u32(b, i);
    const name = String.fromCharCode(b[i + 4], b[i + 5], b[i + 6], b[i + 7]);
    const data = b.subarray(i + 8, i + 8 + len);
    if (name === 'IHDR') {
      w = u32(data, 0);
      h = u32(data, 4);
      depth = data[8];
      type = data[9];
      interlace = data[12];
    } else if (name === 'PLTE') palette = data;
    else if (name === 'tRNS') trns = data;
    else if (name === 'IDAT') parts.push(data);
    else if (name === 'IEND') break;
    i += 12 + len;
  }
  if (!w || !h || !(type in CHANNELS)) throw new Error('not a PNG this code reads');
  if (interlace) throw new Error('interlaced PNGs are not supported');
  const joined = new Uint8Array(parts.reduce((a, p) => a + p.length, 0));
  let at = 0;
  for (const p of parts) {
    joined.set(p, at);
    at += p.length;
  }
  const raw = await inflate(joined);
  const channels = CHANNELS[type];
  const bits = channels * depth;
  const stride = Math.ceil((w * bits) / 8);
  const bpp = Math.max(1, bits >> 3);
  if (raw.length < h * (stride + 1)) throw new Error('the PNG is cut short');
  const px = new Uint8Array(h * stride);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)];
    const src = y * (stride + 1) + 1;
    const dst = y * stride;
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? px[dst + x - bpp] : 0;
      const up = y ? px[dst - stride + x] : 0;
      const c = y && x >= bpp ? px[dst - stride + x - bpp] : 0;
      let v = raw[src + x];
      if (f === 1) v += a;
      else if (f === 2) v += up;
      else if (f === 3) v += (a + up) >> 1;
      else if (f === 4) {
        const p = a + up - c;
        const pa = Math.abs(p - a);
        const pb = Math.abs(p - up);
        const pc = Math.abs(p - c);
        v += pa <= pb && pa <= pc ? a : pb <= pc ? up : c;
      }
      px[dst + x] = v & 255;
    }
  }
  const rgb = new Uint8Array(w * h * 3);
  const hasAlpha = type === 4 || type === 6 || (type === 3 && !!trns);
  const alpha = hasAlpha ? new Uint8Array(w * h) : undefined;
  const scale = depth < 8 ? 255 / ((1 << depth) - 1) : 1;
  /** Sample `s` (0-based, within a row) of row `y`, as it is stored. */
  const sample = (y: number, s: number): number => {
    const row = y * stride;
    if (depth === 8) return px[row + s];
    if (depth === 16) return px[row + s * 2];
    const perByte = 8 / depth;
    const shift = 8 - depth * ((s % perByte) + 1);
    return (px[row + Math.floor(s / perByte)] >> shift) & ((1 << depth) - 1);
  };
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const o = (y * w + x) * 3;
      if (type === 3) {
        const i = sample(y, x);
        rgb[o] = palette?.[i * 3] ?? 0;
        rgb[o + 1] = palette?.[i * 3 + 1] ?? 0;
        rgb[o + 2] = palette?.[i * 3 + 2] ?? 0;
        if (alpha) alpha[y * w + x] = trns![i] ?? 255;
      } else if (type === 0 || type === 4) {
        const g = Math.round(sample(y, x * channels) * scale);
        rgb[o] = rgb[o + 1] = rgb[o + 2] = g;
        if (alpha) alpha[y * w + x] = sample(y, x * channels + 1);
      } else {
        rgb[o] = sample(y, x * channels);
        rgb[o + 1] = sample(y, x * channels + 1);
        rgb[o + 2] = sample(y, x * channels + 2);
        if (alpha) alpha[y * w + x] = sample(y, x * channels + 3);
      }
    }
  }
  return { width: w, height: h, rgb, alpha };
}

/** The components, size and color model of a JPEG, for the PDF to embed it as it is. */
export function jpegHeader(b: Uint8Array): { width: number; height: number; components: number; adobe: boolean } | null {
  let i = 2;
  let adobe = false;
  while (i + 9 < b.length) {
    if (b[i] !== 0xff) {
      i++;
      continue;
    }
    const marker = b[i + 1];
    if (marker === 0xff) {
      i++;
      continue;
    }
    if (marker === 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker === 0x01) {
      i += 2;
      continue;
    }
    const len = u16(b, i + 2);
    if (marker === 0xee && len >= 12 && String.fromCharCode(...b.subarray(i + 4, i + 9)) === 'Adobe') adobe = true;
    if (marker >= 0xc0 && marker <= 0xcf && marker !== 0xc4 && marker !== 0xc8 && marker !== 0xcc) return { height: u16(b, i + 5), width: u16(b, i + 7), components: b[i + 9], adobe };
    i += 2 + len;
  }
  return null;
}
