// A minimal libbufs message builder, enough for the protocol's Request: the same allocation rules as
// include/bufs/builder.hpp (front to back, tables 8-aligned, vectors 4-aligned or 4 mod 8, forward references,
// everything padded with zeros), so a message built here is byte for byte the one the C++ builder makes from the
// same calls in the same order.

const align4 = (n: number): number => (n + 3) & ~3;

export class MessageBuilder {
  private bytes = new Uint8Array(256);
  private view = new DataView(this.bytes.buffer);
  private used = 0;
  private readonly encoder = new TextEncoder();

  /** Starts a message whose root is a table of the given type; returns the offset of the root table. */
  constructor(typeId: number, rootDataBytes: number, rootRefCount: number) {
    this.allocate(8, 8, 0);
    this.view.setUint32(4, typeId, true);
    this.table(rootDataBytes, rootRefCount);
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
    this.view = new DataView(grown.buffer);
  }

  /** A table: its u16 dataBytes and refCount header written, the rest zero. */
  table(dataBytes: number, refCount: number): number {
    const at = this.allocate(align4(dataBytes) + 4 * refCount, 8, 0);
    this.view.setUint16(at, dataBytes, true);
    this.view.setUint16(at + 2, refCount, true);
    return at;
  }

  /** A vector of `count` elements of `elementBytes`; 8-byte elements are 8-aligned. */
  vector(count: number, elementBytes: number, elementAlign: number): number {
    const bytes = 4 + count * elementBytes;
    const at = elementAlign === 8 ? this.allocate(bytes, 8, 4) : this.allocate(bytes, 4, 0);
    this.view.setUint32(at, count, true);
    return at;
  }

  /** A string: u32 length, the UTF-8 bytes, a NUL. */
  string(text: string): number {
    const encoded = this.encoder.encode(text);
    const at = this.allocate(4 + encoded.length + 1, 4, 0);
    this.view.setUint32(at, encoded.length, true);
    this.bytes.set(encoded, at + 4);
    return at;
  }

  /** A vector of bytes, copied in. */
  byteVector(data: Uint8Array): number {
    const at = this.vector(data.length, 1, 4);
    this.bytes.set(data, at + 4);
    return at;
  }

  /** Points the reference slot of `table` (index `refIndex`, data section `dataBytes`) at `target`. */
  setRef(table: number, dataBytes: number, refIndex: number, target: number): void {
    const slot = table + align4(dataBytes) + 4 * refIndex;
    this.view.setUint32(slot, target - slot, true);
  }

  /** Points element `index` of the reference vector at `vector` to `target`. */
  setElementRef(vector: number, index: number, target: number): void {
    const slot = vector + 4 + 4 * index;
    this.view.setUint32(slot, target - slot, true);
  }

  u8(at: number, value: number): void {
    this.view.setUint8(at, value);
  }
  u16(at: number, value: number): void {
    this.view.setUint16(at, value, true);
  }
  u32(at: number, value: number): void {
    this.view.setUint32(at, value, true);
  }
  u64(at: number, value: bigint | number): void {
    this.view.setBigUint64(at, BigInt(value), true);
  }
  i64(at: number, value: bigint | number): void {
    this.view.setBigInt64(at, BigInt(value), true);
  }
  f64(at: number, value: number): void {
    this.view.setFloat64(at, value, true);
  }
  bit(byteAt: number, bit: number, on: boolean): void {
    if (on) {
      this.view.setUint8(byteAt, this.view.getUint8(byteAt) | (1 << bit));
    }
  }

  /** Seals the message: pads to 8, writes the size, returns the bytes. */
  finish(): ArrayBuffer {
    const end = this.allocate(0, 8, 0);
    this.view.setUint32(0, end, true);
    return this.bytes.slice(0, end).buffer;
  }
}
