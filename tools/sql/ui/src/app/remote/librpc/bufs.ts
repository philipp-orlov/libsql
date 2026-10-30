// Vendored from libbufs/ts/src by scripts/vendor-librpc.mjs. Do not edit here.
// Copyright (c) 2026 Philipp Orlov

// libbufs messages in TypeScript: the reader (Table) and builder (MessageBuilder) the code `bufsc --ts` generates is
// written against. The format is described in libbufs/docs/design.md: little-endian, 8-aligned, references are forward offsets from their own slot.
//
// Reading is bounds-checked by DataView (RangeError), so a malformed message cannot read outside itself.
// It is not "verified" the way the C++ verifier does; an application that treats a peer as hostile should
// catch RangeError around decoding and reading.

const align4 = (n: number): number => (n + 3) & ~3;

export const HEADER_BYTES = 8;

const textDecoder = new TextDecoder();
const textEncoder = new TextEncoder();
const scratch = new DataView(new ArrayBuffer(8));

export class MalformedMessage extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'MalformedMessage';
  }
}

/** A vector in the message: where its elements start (an offset in the DataView) and how many there are. */
export interface VectorRef {
  start: number;
  count: number;
}

export function toView(data: ArrayBuffer | ArrayBufferView): DataView {
  return ArrayBuffer.isView(data) ? new DataView(data.buffer, data.byteOffset, data.byteLength) : new DataView(data);
}

/** The type id in a message's header, or 0 when it is too short. */
export function messageType(data: ArrayBuffer | ArrayBufferView): number {
  const view = toView(data);
  return view.byteLength >= HEADER_BYTES ? view.getUint32(4, true) : 0;
}

/** The size a message declares in its header (the bytes it occupies, a frame may hold more). */
export function messageSize(data: ArrayBuffer | ArrayBufferView): number {
  const view = toView(data);
  return view.byteLength >= 4 ? view.getUint32(0, true) : 0;
}

/** Checks the header of `data` and returns its root table. */
export function rootTable(data: ArrayBuffer | ArrayBufferView, typeId: number, typeName = 'message'): Table {
  const view = toView(data);
  if (view.byteLength < HEADER_BYTES + 4) {
    throw new MalformedMessage(`${typeName}: message is too short`);
  }
  const size = view.getUint32(0, true);
  if (size < HEADER_BYTES + 4 || size > view.byteLength || size % 8 !== 0) {
    throw new MalformedMessage(`${typeName}: bad message size`);
  }
  const actual = view.getUint32(4, true);
  if (actual !== typeId) {
    throw new MalformedMessage(`${typeName}: wrong message type 0x${actual.toString(16)}`);
  }
  return new Table(view, HEADER_BYTES);
}

function f32FromBits(bits: number): number {
  scratch.setUint32(0, bits >>> 0, true);
  return scratch.getFloat32(0, true);
}

function f64FromBits(bits: bigint): number {
  scratch.setBigUint64(0, BigInt.asUintN(64, bits), true);
  return scratch.getFloat64(0, true);
}

export class Table {
  readonly dataBytes: number;
  readonly refCount: number;

  constructor(
    readonly view: DataView,
    readonly at: number,
  ) {
    this.dataBytes = view.getUint16(at, true);
    this.refCount = view.getUint16(at + 2, true);
  }

  // A scalar is stored XOR its schema default. A field past the writer's data section reads as 0, which
  // XOR the default is the default.

  u8(offset: number, dflt = 0): number {
    return (offset + 1 <= this.dataBytes ? this.view.getUint8(this.at + offset) : 0) ^ dflt;
  }
  i8(offset: number, dflt = 0): number {
    return (offset + 1 <= this.dataBytes ? this.view.getInt8(this.at + offset) : 0) ^ dflt;
  }
  u16(offset: number, dflt = 0): number {
    return (offset + 2 <= this.dataBytes ? this.view.getUint16(this.at + offset, true) : 0) ^ dflt;
  }
  i16(offset: number, dflt = 0): number {
    return (offset + 2 <= this.dataBytes ? this.view.getInt16(this.at + offset, true) : 0) ^ dflt;
  }
  u32(offset: number, dflt = 0): number {
    return ((offset + 4 <= this.dataBytes ? this.view.getUint32(this.at + offset, true) : 0) ^ dflt) >>> 0;
  }
  i32(offset: number, dflt = 0): number {
    return (offset + 4 <= this.dataBytes ? this.view.getInt32(this.at + offset, true) : 0) ^ dflt;
  }
  u64(offset: number, dflt = 0n): bigint {
    const raw = offset + 8 <= this.dataBytes ? this.view.getBigUint64(this.at + offset, true) : 0n;
    return BigInt.asUintN(64, raw ^ dflt);
  }
  i64(offset: number, dflt = 0n): bigint {
    const raw = offset + 8 <= this.dataBytes ? this.view.getBigInt64(this.at + offset, true) : 0n;
    return BigInt.asIntN(64, raw ^ dflt);
  }
  /** `dfltBits`: the bit pattern of the default. */
  f32(offset: number, dfltBits = 0): number {
    return f32FromBits(((offset + 4 <= this.dataBytes ? this.view.getUint32(this.at + offset, true) : 0) ^ dfltBits) >>> 0);
  }
  f64(offset: number, dfltBits = 0n): number {
    const raw = offset + 8 <= this.dataBytes ? this.view.getBigUint64(this.at + offset, true) : 0n;
    return f64FromBits(raw ^ dfltBits);
  }
  bit(byte: number, bit: number, dflt = 0): boolean {
    return (((this.u8(byte) >> bit) & 1) ^ dflt) === 1;
  }

  /** Absolute offset of an inline struct of `size` bytes at `offset`, or -1 when the writer did not have it. */
  structAt(offset: number, size: number): number {
    return offset + size <= this.dataBytes ? this.at + offset : -1;
  }

  /** Absolute offset of the object reference `index` points to, or -1 when it is null or absent. */
  ref(index: number): number {
    if (index >= this.refCount) {
      return -1;
    }
    const slot = this.at + align4(this.dataBytes) + 4 * index;
    const relative = this.view.getUint32(slot, true);
    return relative === 0 ? -1 : slot + relative;
  }

  vector(index: number): VectorRef | null {
    const target = this.ref(index);
    return target < 0 ? null : { start: target + 4, count: this.view.getUint32(target, true) };
  }

  string(index: number): string {
    const target = this.ref(index);
    if (target < 0) {
      return '';
    }
    return decodeString(this.view, target);
  }

  table(index: number): Table | null {
    const target = this.ref(index);
    return target < 0 ? null : new Table(this.view, target);
  }
}

function decodeString(view: DataView, target: number): string {
  const length = view.getUint32(target, true);
  if (target + 4 + length > view.byteLength) {
    throw new RangeError('string runs past the end of the message');
  }
  return textDecoder.decode(new Uint8Array(view.buffer, view.byteOffset + target + 4, length));
}

function checkVector(view: DataView, v: VectorRef, elementBytes: number): void {
  if (v.start + v.count * elementBytes > view.byteLength) {
    throw new RangeError('vector runs past the end of the message');
  }
}

/** The elements of a vector of scalars, enums or structs, read with `read(view, absoluteOffset)`. */
export function readList<T>(
  view: DataView,
  v: VectorRef | null,
  elementBytes: number,
  read: (view: DataView, at: number) => T,
): T[] {
  if (!v) {
    return [];
  }
  checkVector(view, v, elementBytes);
  const out = new Array<T>(v.count);
  for (let i = 0; i < v.count; i++) {
    out[i] = read(view, v.start + i * elementBytes);
  }
  return out;
}

/** The same for 64-bit elements (bigint). */
export function readBigList(
  view: DataView,
  v: VectorRef | null,
  elementBytes: number,
  read: (view: DataView, at: number) => bigint,
): bigint[] {
  return readList(view, v, elementBytes, read);
}

/** A vector of bytes, copied out. */
export function readBytes(view: DataView, v: VectorRef | null): Uint8Array {
  if (!v) {
    return new Uint8Array(0);
  }
  checkVector(view, v, 1);
  return new Uint8Array(view.buffer.slice(view.byteOffset + v.start, view.byteOffset + v.start + v.count));
}

export function readStrings(view: DataView, v: VectorRef | null): string[] {
  if (!v) {
    return [];
  }
  checkVector(view, v, 4);
  const out: string[] = [];
  for (let i = 0; i < v.count; i++) {
    const slot = v.start + 4 * i;
    const relative = view.getUint32(slot, true);
    out.push(relative === 0 ? '' : decodeString(view, slot + relative));
  }
  return out;
}

export function readTables(view: DataView, v: VectorRef | null): Table[] {
  if (!v) {
    return [];
  }
  checkVector(view, v, 4);
  const out: Table[] = [];
  for (let i = 0; i < v.count; i++) {
    const slot = v.start + 4 * i;
    out.push(new Table(view, slot + view.getUint32(slot, true)));
  }
  return out;
}

/**
 * Builds one message front to back with the allocation rules of include/bufs/builder.hpp: tables 8-aligned,
 * vectors 4-aligned (4 mod 8 when their elements are 8 bytes), forward references, padding zeroed.
 */
export class MessageBuilder {
  private bytes = new Uint8Array(256);
  private dv = new DataView(this.bytes.buffer);
  private used = 0;

  constructor(typeId: number) {
    this.allocate(HEADER_BYTES, 8, 0);
    this.dv.setUint32(4, typeId, true);
  }

  /** The current DataView (it changes when the buffer grows: do not keep it across allocations). */
  get view(): DataView {
    return this.dv;
  }

  /** `bytes` at an offset that is `phase` mod `align` (4 or 8; phase 0 or 4). */
  allocate(bytes: number, align: number, phase: number): number {
    let at = this.used;
    while (at % align !== phase) {
      at += 4;
    }
    const end = align4(at + bytes);
    this.reserve(end);
    this.used = end;
    return at;
  }

  private reserve(size: number): void {
    if (size <= this.bytes.length) {
      return;
    }
    const grown = new Uint8Array(Math.max(size, this.bytes.length * 2));
    grown.set(this.bytes);
    this.bytes = grown;
    this.dv = new DataView(grown.buffer);
  }

  /** A table: its u16 dataBytes and refCount header written, the rest zero. */
  table(dataBytes: number, refCount: number): number {
    const at = this.allocate(align4(dataBytes) + 4 * refCount, 8, 0);
    this.dv.setUint16(at, dataBytes, true);
    this.dv.setUint16(at + 2, refCount, true);
    return at;
  }

  /** A vector of `count` elements of `elementBytes`; 8-byte elements are 8-aligned. */
  vector(count: number, elementBytes: number, elementAlign: number): number {
    const at =
      elementAlign === 8
        ? this.allocate(4 + count * elementBytes, 8, 4)
        : this.allocate(4 + count * elementBytes, 4, 0);
    this.dv.setUint32(at, count, true);
    return at;
  }

  /** A string: u32 length, the UTF-8 bytes, a NUL. */
  string(text: string): number {
    const encoded = textEncoder.encode(text);
    const at = this.allocate(4 + encoded.length + 1, 4, 0);
    this.dv.setUint32(at, encoded.length, true);
    this.bytes.set(encoded, at + 4);
    return at;
  }

  /** Points reference slot `refIndex` of `table` (data section `dataBytes`) at `target`. */
  setRef(table: number, dataBytes: number, refIndex: number, target: number): void {
    const slot = table + align4(dataBytes) + 4 * refIndex;
    this.dv.setUint32(slot, target - slot, true);
  }

  /** Points element `index` of the reference vector at `vector` to `target`. */
  setElementRef(vector: number, index: number, target: number): void {
    const slot = vector + 4 + 4 * index;
    this.dv.setUint32(slot, target - slot, true);
  }

  // Scalars are stored XOR their schema default.
  setU8(at: number, value: number, dflt = 0): void {
    this.dv.setUint8(at, value ^ dflt);
  }
  setI8(at: number, value: number, dflt = 0): void {
    this.dv.setInt8(at, value ^ dflt);
  }
  setU16(at: number, value: number, dflt = 0): void {
    this.dv.setUint16(at, (value ^ dflt) & 0xffff, true);
  }
  setI16(at: number, value: number, dflt = 0): void {
    this.dv.setInt16(at, value ^ dflt, true);
  }
  setU32(at: number, value: number, dflt = 0): void {
    this.dv.setUint32(at, (value ^ dflt) >>> 0, true);
  }
  setI32(at: number, value: number, dflt = 0): void {
    this.dv.setInt32(at, value ^ dflt, true);
  }
  setU64(at: number, value: bigint | number, dflt = 0n): void {
    this.dv.setBigUint64(at, BigInt.asUintN(64, BigInt(value) ^ dflt), true);
  }
  setI64(at: number, value: bigint | number, dflt = 0n): void {
    this.dv.setBigInt64(at, BigInt.asIntN(64, BigInt(value) ^ dflt), true);
  }
  setF32(at: number, value: number, dfltBits = 0): void {
    scratch.setFloat32(0, value, true);
    this.dv.setUint32(at, (scratch.getUint32(0, true) ^ dfltBits) >>> 0, true);
  }
  setF64(at: number, value: number, dfltBits = 0n): void {
    scratch.setFloat64(0, value, true);
    this.dv.setBigUint64(at, BigInt.asUintN(64, scratch.getBigUint64(0, true) ^ dfltBits), true);
  }
  setBit(byteAt: number, bit: number, value: boolean, dflt = 0): void {
    const stored = (value ? 1 : 0) ^ dflt;
    this.dv.setUint8(byteAt, (this.dv.getUint8(byteAt) & ~(1 << bit)) | (stored << bit));
  }

  /** Seals the message: pads to 8, writes the size, returns the bytes (a copy). */
  finish(): Uint8Array {
    const end = this.allocate(0, 8, 0);
    this.dv.setUint32(0, end, true);
    return this.bytes.slice(0, end);
  }
}
