// Reading what sql::BufsWriter wrote (include/sql/wire/resultset.bufs), with the same method names as the C++
// reader (include/sql/wire/result_view.hpp) and the cell rules of the format (section 3.7): typed-array views
// over the received bytes, no parsing, text decoded only for the cells that are asked for.
//
// Free of Angular imports on purpose: this folder can become its own package.

import type { Cell } from '../models/api.models';
import { COLUMN, COLUMN_INFO, Encoding, HEADER_BYTES, RESULT_SET, RESULT_SET_TYPE_ID, Scale, Tag, Width } from './layout';
import { Table, VectorRef } from './message';

export class BufsError extends Error {
  constructor(message = 'not a libsql bufs result') {
    super(message);
    this.name = 'BufsError';
  }
}

const MAX_SAFE = BigInt(Number.MAX_SAFE_INTEGER);
const BLOB_PREVIEW_BYTES = 32;
const UNIT_MICROS: Record<Scale, bigint> = {
  [Scale.Micro]: 1n,
  [Scale.Milli]: 1000n,
  [Scale.Second]: 1000000n,
  [Scale.Day]: 86400000000n,
};

/** The text `sql::formatDatetime` produces: `2024-03-01 12:34:56`, plus `.ffffff` when the fraction is nonzero. */
export function formatDatetime(micros: bigint): string {
  let seconds = micros / 1000000n;
  let fraction = micros % 1000000n;
  if (fraction < 0n) {
    fraction += 1000000n;
    seconds -= 1n;
  }
  const iso = new Date(Number(seconds) * 1000).toISOString();
  const text = `${iso.slice(0, 10)} ${iso.slice(11, 19)}`;
  return fraction === 0n ? text : `${text}.${fraction.toString().padStart(6, '0')}`;
}

function hex(bytes: Uint8Array, count: number): string {
  let out = '';
  for (let i = 0; i < Math.min(count, bytes.length); i++) {
    out += bytes[i].toString(16).padStart(2, '0');
  }
  return out;
}

/** One column of one batch. */
export class ColumnView {
  readonly encoding: Encoding;
  readonly width: Width;
  readonly scale: Scale;
  readonly nullCount: number;
  readonly base: bigint;

  private readonly decoder = new TextDecoder('utf-8', { fatal: true });
  private cache: Record<string, ArrayBufferView | null> = {};
  private slots: Uint32Array | null = null;
  private strings: (string | undefined)[] | null = null;
  private dictStrings: (string | undefined)[] | null = null;
  private fastNumbers: boolean;

  constructor(
    private readonly table: Table,
    readonly size: number,
  ) {
    this.encoding = table.u8(COLUMN.encoding) as Encoding;
    this.width = table.u8(COLUMN.width) as Width;
    this.scale = table.u8(COLUMN.scale) as Scale;
    this.nullCount = table.u32(COLUMN.nullCount);
    this.base = table.i64(COLUMN.base);
    // base + a 32-bit delta stays an exact double.
    const magnitude = this.base < 0n ? -this.base : this.base;
    this.fastNumbers = magnitude + 0x100000000n <= MAX_SAFE;
  }

  // --- typed views, created once -------------------------------------------------------------

  private typed<T extends ArrayBufferView>(name: string, index: number, make: (buffer: ArrayBuffer, offset: number, count: number) => T): T | null {
    if (name in this.cache) {
      return this.cache[name] as T | null;
    }
    const vector: VectorRef | null = this.table.vector(index);
    const view = this.table.view;
    const out = vector ? make(view.buffer as ArrayBuffer, view.byteOffset + vector.start, vector.count) : null;
    this.cache[name] = out;
    return out;
  }

  get u8(): Uint8Array | null {
    return this.typed('u8', COLUMN.u8, (b, o, n) => new Uint8Array(b, o, n));
  }
  get u16(): Uint16Array | null {
    return this.typed('u16', COLUMN.u16, (b, o, n) => new Uint16Array(b, o, n));
  }
  get u32(): Uint32Array | null {
    return this.typed('u32', COLUMN.u32, (b, o, n) => new Uint32Array(b, o, n));
  }
  get i64(): BigInt64Array | null {
    return this.typed('i64', COLUMN.i64, (b, o, n) => new BigInt64Array(b, o, n));
  }
  get f32(): Float32Array | null {
    return this.typed('f32', COLUMN.f32, (b, o, n) => new Float32Array(b, o, n));
  }
  get f64(): Float64Array | null {
    return this.typed('f64', COLUMN.f64, (b, o, n) => new Float64Array(b, o, n));
  }
  get offsets(): Uint32Array | null {
    return this.typed('offsets', COLUMN.offsets, (b, o, n) => new Uint32Array(b, o, n));
  }
  get bytes(): Uint8Array | null {
    return this.typed('bytes', COLUMN.bytes, (b, o, n) => new Uint8Array(b, o, n));
  }
  get dictOffsets(): Uint32Array | null {
    return this.typed('dictOffsets', COLUMN.dictOffsets, (b, o, n) => new Uint32Array(b, o, n));
  }
  get dictBytes(): Uint8Array | null {
    return this.typed('dictBytes', COLUMN.dictBytes, (b, o, n) => new Uint8Array(b, o, n));
  }
  get tags(): Uint8Array | null {
    return this.typed('tags', COLUMN.tags, (b, o, n) => new Uint8Array(b, o, n));
  }
  get blobSizes(): Uint32Array | null {
    return this.typed('blobSizes', COLUMN.blobSizes, (b, o, n) => new Uint32Array(b, o, n));
  }
  private get nulls(): Uint8Array | null {
    return this.typed('nulls', COLUMN.nulls, (b, o, n) => new Uint8Array(b, o, n));
  }

  // --- cells ----------------------------------------------------------------------------------

  isNull(row: number): boolean {
    if (this.encoding === Encoding.Empty) {
      return true;
    }
    if (this.encoding === Encoding.Mixed) {
      return this.tags![row] === Tag.Null;
    }
    return this.nullCount > 0 && ((this.nulls![row >> 3] >> (row & 7)) & 1) === 1;
  }

  /** The storage class of the cell. */
  tag(row: number): Tag {
    if (this.isNull(row)) {
      return Tag.Null;
    }
    switch (this.encoding) {
      case Encoding.Int:
        return Tag.Integer;
      case Encoding.Real:
        return Tag.Real;
      case Encoding.Datetime:
        return Tag.Datetime;
      case Encoding.Text:
      case Encoding.TextDict:
        return Tag.Text;
      case Encoding.Blob:
        return Tag.Blob;
      case Encoding.Mixed:
        return this.tags![row] as Tag;
      default:
        return Tag.Null;
    }
  }

  private delta(row: number): number {
    switch (this.width) {
      case Width.U8:
        return this.u8![row];
      case Width.U16:
        return this.u16![row];
      default:
        return this.u32![row];
    }
  }

  /** Index of the row within its pool in a Mixed column (integers and datetimes, reals, or texts and blobs). */
  private slot(row: number): number {
    if (!this.slots) {
      const tags = this.tags!;
      const slots = new Uint32Array(this.size);
      let ints = 0;
      let reals = 0;
      let bytes = 0;
      for (let i = 0; i < this.size; i++) {
        switch (tags[i]) {
          case Tag.Integer:
          case Tag.Datetime:
            slots[i] = ints++;
            break;
          case Tag.Real:
            slots[i] = reals++;
            break;
          case Tag.Text:
          case Tag.Blob:
            slots[i] = bytes++;
            break;
        }
      }
      this.slots = slots;
    }
    return this.slots[row];
  }

  /** An INTEGER cell, always exact. */
  bigint(row: number): bigint {
    if (this.encoding === Encoding.Mixed) {
      return this.i64![this.slot(row)];
    }
    if (this.width === Width.I64) {
      return this.i64![row];
    }
    return this.base + BigInt(this.delta(row));
  }

  /** An INTEGER cell as a number: exact up to 2^53, so check `Number.isSafeInteger` or use `bigint()` beyond. */
  number(row: number): number {
    if (this.encoding === Encoding.Int && this.width !== Width.I64 && this.fastNumbers) {
      return Number(this.base) + this.delta(row);
    }
    return Number(this.bigint(row));
  }

  real(row: number): number {
    if (this.encoding === Encoding.Mixed) {
      return this.f64![this.slot(row)];
    }
    const f32 = this.f32;
    return f32 ? f32[row] : this.f64![row];
  }

  /** Microseconds since the Unix epoch (UTC). */
  datetime(row: number): bigint {
    if (this.encoding === Encoding.Mixed) {
      return this.i64![this.slot(row)];
    }
    if (this.width === Width.I64) {
      return this.i64![row];
    }
    return this.base + BigInt(this.delta(row)) * UNIT_MICROS[this.scale];
  }

  /** The bytes of a TEXT or BLOB cell (a BLOB may be a prefix, see `blobSize`). */
  raw(row: number): Uint8Array {
    if (this.encoding === Encoding.TextDict) {
      const code = this.width === Width.U8 ? this.u8![row] : this.u16![row];
      const offsets = this.dictOffsets!;
      return this.dictBytes!.subarray(offsets[code], offsets[code + 1]);
    }
    const i = this.encoding === Encoding.Mixed ? this.slot(row) : row;
    const offsets = this.offsets!;
    return this.bytes ? this.bytes.subarray(offsets[i], offsets[i + 1]) : new Uint8Array(0);
  }

  /** The full size of a BLOB cell (the stored length when nothing was cut). */
  blobSize(row: number): number {
    const sizes = this.blobSizes;
    if (!sizes) {
      return this.raw(row).length;
    }
    return sizes[this.encoding === Encoding.Mixed ? this.slot(row) : row];
  }

  /** A TEXT cell decoded as UTF-8; throws on invalid UTF-8 (see `cell`, which turns that into a blob cell). */
  text(row: number): string {
    if (this.encoding === Encoding.TextDict) {
      const code = this.width === Width.U8 ? this.u8![row] : this.u16![row];
      this.dictStrings ??= new Array(this.dictOffsets!.length - 1);
      return (this.dictStrings[code] ??= this.decoder.decode(this.raw(row)));
    }
    this.strings ??= new Array(this.size);
    return (this.strings[row] ??= this.decoder.decode(this.raw(row)));
  }

  /** The cell as the web API's JSON would send it, for the grid. */
  cell(row: number): Cell {
    switch (this.tag(row)) {
      case Tag.Null:
        return null;
      case Tag.Integer: {
        const n = this.bigint(row);
        return n >= -MAX_SAFE && n <= MAX_SAFE ? Number(n) : { type: 'integer', value: n.toString() };
      }
      case Tag.Real: {
        const d = this.real(row);
        return Number.isFinite(d) ? d : Number.isNaN(d) ? 'NaN' : d > 0 ? 'Inf' : '-Inf';
      }
      case Tag.Datetime:
        return formatDatetime(this.datetime(row));
      case Tag.Text:
        try {
          return this.text(row);
        } catch {
          const raw = this.raw(row);
          return { type: 'blob', size: raw.length, value: hex(raw, BLOB_PREVIEW_BYTES) };
        }
      default:
        return { type: 'blob', size: this.blobSize(row), value: hex(this.raw(row), BLOB_PREVIEW_BYTES) };
    }
  }
}

/** One message: a range of rows. */
export class BatchView {
  readonly rowOffset: number;
  readonly rowCount: number;
  readonly more: boolean;
  readonly truncated: boolean;
  readonly totalRows: number;
  readonly changes: number;
  readonly lastInsertId: number;
  private readonly columnViews: ColumnView[] = [];
  private readonly root: Table;

  constructor(view: DataView, messageOffset: number) {
    this.root = new Table(view, messageOffset + HEADER_BYTES);
    const r = this.root;
    this.rowOffset = Number(r.u64(RESULT_SET.rowOffset));
    this.rowCount = r.u32(RESULT_SET.rowCount);
    this.more = r.bit(RESULT_SET.more.byte, RESULT_SET.more.bit);
    this.truncated = r.bit(RESULT_SET.truncated.byte, RESULT_SET.truncated.bit);
    this.totalRows = Number(r.u64(RESULT_SET.totalRows));
    this.changes = Number(r.u64(RESULT_SET.changes));
    this.lastInsertId = Number(r.i64(RESULT_SET.lastInsertId));
  }

  get columnCount(): number {
    return this.root.vector(RESULT_SET.data)?.count ?? 0;
  }

  column(i: number): ColumnView {
    let view = this.columnViews[i];
    if (!view) {
      const data = this.root.vector(RESULT_SET.data);
      if (!data || i >= data.count) {
        throw new RangeError(`no column ${i}`);
      }
      view = new ColumnView(this.root.tableAt(data, i), this.rowCount);
      this.columnViews[i] = view;
    }
    return view;
  }

  columnInfos(): { name: string; qualified: string; declType: string }[] {
    const infos = this.root.vector(RESULT_SET.columns);
    if (!infos) {
      return [];
    }
    const out = [];
    for (let i = 0; i < infos.count; i++) {
      const t = this.root.tableAt(infos, i);
      out.push({ name: t.string(COLUMN_INFO.name), qualified: t.string(COLUMN_INFO.qualified), declType: t.string(COLUMN_INFO.declType) });
    }
    return out;
  }
}

/** All batches of a stream, with the column descriptions of the first. */
export class ResultSetView {
  readonly columns: string[];
  readonly qualified: string[];
  readonly declTypes: string[];
  readonly rowCount: number;
  readonly truncated: boolean;
  readonly totalRows: number;
  readonly changes: number;
  readonly lastInsertId: number;

  private constructor(private readonly batches: BatchView[]) {
    const infos = batches[0].columnInfos();
    this.columns = infos.map((c) => c.name);
    this.qualified = infos.map((c) => c.qualified);
    this.declTypes = infos.map((c) => c.declType);
    const last = batches[batches.length - 1];
    this.rowCount = last.rowOffset + last.rowCount;
    this.truncated = last.truncated;
    this.totalRows = last.totalRows;
    this.changes = last.changes;
    this.lastInsertId = last.lastInsertId;
  }

  /**
   * Wraps a stream of messages. The bytes must start 8-aligned inside `buffer` (an ArrayBuffer from fetch always
   * does); otherwise they are copied first.
   */
  static open(buffer: ArrayBuffer, byteOffset = 0, byteLength: number = buffer.byteLength - byteOffset): ResultSetView {
    try {
      if (byteOffset % 8 !== 0) {
        const aligned = new Uint8Array(byteLength);
        aligned.set(new Uint8Array(buffer, byteOffset, byteLength));
        buffer = aligned.buffer;
        byteOffset = 0;
      }
      const view = new DataView(buffer, byteOffset, byteLength);
      const batches: BatchView[] = [];
      let at = 0;
      while (at < byteLength) {
        if (byteLength - at < HEADER_BYTES) {
          throw new BufsError();
        }
        const size = view.getUint32(at, true);
        if (size < HEADER_BYTES || size % 8 !== 0 || at + size > byteLength || view.getUint32(at + 4, true) !== RESULT_SET_TYPE_ID) {
          throw new BufsError();
        }
        batches.push(new BatchView(view, at));
        at += size;
      }
      if (batches.length === 0) {
        throw new BufsError();
      }
      return new ResultSetView(batches);
    } catch (e) {
      throw e instanceof BufsError ? e : new BufsError();
    }
  }

  get batchCount(): number {
    return this.batches.length;
  }

  batch(i: number): BatchView {
    return this.batches[i];
  }

  /** The batch holding `row`, found by binary search on rowOffset. */
  private batchOf(row: number): BatchView {
    let lo = 0;
    let hi = this.batches.length;
    while (hi - lo > 1) {
      const mid = (lo + hi) >> 1;
      if (this.batches[mid].rowOffset <= row) {
        lo = mid;
      } else {
        hi = mid;
      }
    }
    return this.batches[lo];
  }

  /** The cell as the web API's JSON would send it. */
  cell(row: number, column: number): Cell {
    const batch = this.batchOf(row);
    return batch.column(column).cell(row - batch.rowOffset);
  }
}
