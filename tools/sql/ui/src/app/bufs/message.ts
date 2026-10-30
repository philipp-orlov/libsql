// Low-level libbufs reading over a DataView: tables, references, vectors and strings. Scalars past a table's
// dataBytes read as 0 (a field added after the writer's schema), which is also every default in resultset.bufs.
// Never add a non-zero default there: a scalar is stored XOR its default and this reader does not undo that.

const align4 = (n: number): number => (n + 3) & ~3;

/** A vector in the message: where its elements start (absolute offset in the DataView) and how many. */
export interface VectorRef {
  start: number;
  count: number;
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

  u8(offset: number): number {
    return offset + 1 <= this.dataBytes ? this.view.getUint8(this.at + offset) : 0;
  }

  u16(offset: number): number {
    return offset + 2 <= this.dataBytes ? this.view.getUint16(this.at + offset, true) : 0;
  }

  u32(offset: number): number {
    return offset + 4 <= this.dataBytes ? this.view.getUint32(this.at + offset, true) : 0;
  }

  u64(offset: number): bigint {
    return offset + 8 <= this.dataBytes ? this.view.getBigUint64(this.at + offset, true) : 0n;
  }

  i64(offset: number): bigint {
    return offset + 8 <= this.dataBytes ? this.view.getBigInt64(this.at + offset, true) : 0n;
  }

  bit(byte: number, bit: number): boolean {
    return ((this.u8(byte) >> bit) & 1) === 1;
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
    const length = this.view.getUint32(target, true);
    return new TextDecoder().decode(new Uint8Array(this.view.buffer, this.view.byteOffset + target + 4, length));
  }

  table(index: number): Table | null {
    const target = this.ref(index);
    return target < 0 ? null : new Table(this.view, target);
  }

  /** Element `i` of a vector of strings. */
  stringAt(vector: VectorRef, i: number): string {
    const slot = vector.start + 4 * i;
    const relative = this.view.getUint32(slot, true);
    if (relative === 0) {
      return '';
    }
    const target = slot + relative;
    const length = this.view.getUint32(target, true);
    return new TextDecoder().decode(new Uint8Array(this.view.buffer, this.view.byteOffset + target + 4, length));
  }

  /** Element `i` of the vector of tables at reference `index`. */
  tableAt(vector: VectorRef, i: number): Table {
    const slot = vector.start + 4 * i;
    return new Table(this.view, slot + this.view.getUint32(slot, true));
  }
}
