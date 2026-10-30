// Copyright (c) 2026 Philipp Orlov

// A zip file writer (what .xlsx and .docx are): entries stored, or deflated where the platform can. No libraries.
// Free of Angular imports on purpose.

import { crc32, deflateRaw, inflateRaw } from './compress';

export interface ZipEntry {
  name: string;
  data: Uint8Array | string;
}

const encoder = new TextEncoder();

function dosTime(d: Date): [number, number] {
  const time = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1);
  const date = (Math.max(d.getFullYear() - 1980, 0) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
  return [time, date];
}

export async function buildZip(entries: ZipEntry[], when = new Date()): Promise<Uint8Array> {
  const [time, date] = dosTime(when);
  const parts: Uint8Array[] = [];
  const central: Uint8Array[] = [];
  let offset = 0;
  for (const e of entries) {
    const name = encoder.encode(e.name);
    const raw = typeof e.data === 'string' ? encoder.encode(e.data) : e.data;
    const packed = raw.length > 64 ? await deflateRaw(raw) : null;
    const body = packed && packed.length < raw.length ? packed : raw;
    const method = body === raw ? 0 : 8;
    const crc = crc32(raw);
    const head = new DataView(new ArrayBuffer(30));
    head.setUint32(0, 0x04034b50, true);
    head.setUint16(4, 20, true);
    head.setUint16(6, 0x0800, true); // UTF-8 names
    head.setUint16(8, method, true);
    head.setUint16(10, time, true);
    head.setUint16(12, date, true);
    head.setUint32(14, crc, true);
    head.setUint32(18, body.length, true);
    head.setUint32(22, raw.length, true);
    head.setUint16(26, name.length, true);
    parts.push(new Uint8Array(head.buffer), name, body);
    const dir = new DataView(new ArrayBuffer(46));
    dir.setUint32(0, 0x02014b50, true);
    dir.setUint16(4, 20, true);
    dir.setUint16(6, 20, true);
    dir.setUint16(8, 0x0800, true);
    dir.setUint16(10, method, true);
    dir.setUint16(12, time, true);
    dir.setUint16(14, date, true);
    dir.setUint32(16, crc, true);
    dir.setUint32(20, body.length, true);
    dir.setUint32(24, raw.length, true);
    dir.setUint16(28, name.length, true);
    dir.setUint32(42, offset, true);
    central.push(new Uint8Array(dir.buffer), name);
    offset += 30 + name.length + body.length;
  }
  const size = central.reduce((a, c) => a + c.length, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true);
  end.setUint16(8, entries.length, true);
  end.setUint16(10, entries.length, true);
  end.setUint32(12, size, true);
  end.setUint32(16, offset, true);
  const all = [...parts, ...central, new Uint8Array(end.buffer)];
  const out = new Uint8Array(all.reduce((a, p) => a + p.length, 0));
  let at = 0;
  for (const p of all) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

/** The entries of a zip written by buildZip or anything that stores/deflates (for the specs and for reading back). */
export async function readZip(data: Uint8Array): Promise<Map<string, Uint8Array>> {
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  let end = data.length - 22;
  while (end >= 0 && view.getUint32(end, true) !== 0x06054b50) end--;
  if (end < 0) throw new Error('not a zip file');
  const count = view.getUint16(end + 10, true);
  let at = view.getUint32(end + 16, true);
  const out = new Map<string, Uint8Array>();
  for (let i = 0; i < count; i++) {
    const method = view.getUint16(at + 10, true);
    const csize = view.getUint32(at + 20, true);
    const nlen = view.getUint16(at + 28, true);
    const xlen = view.getUint16(at + 30, true);
    const clen = view.getUint16(at + 32, true);
    const local = view.getUint32(at + 42, true);
    const name = new TextDecoder().decode(data.subarray(at + 46, at + 46 + nlen));
    const start = local + 30 + view.getUint16(local + 26, true) + view.getUint16(local + 28, true);
    const body = data.subarray(start, start + csize);
    out.set(name, method === 8 ? await inflateRaw(body) : body);
    at += 46 + nlen + xlen + clen;
  }
  return out;
}
