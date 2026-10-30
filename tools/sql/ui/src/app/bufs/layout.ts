// Written by hand from include/sql/wire/resultset.bufs. Re-check against `bufsc --layout` whenever the schema
// changes (the golden test, tools/bufs_golden, fails if the C++ writer and this file disagree).
//
// Offsets are bytes from the start of a table (its u16 dataBytes, u16 refCount header included). A reference
// slot `i` sits at align4(dataBytes) + 4 * i and holds a forward offset from the slot itself, 0 = null.

/** Type id of sql.wire.ResultSet in a message header. */
export const RESULT_SET_TYPE_ID = 0x843045e7;

/** u32 size, u32 typeId; the root table follows. */
export const HEADER_BYTES = 8;

export const COLUMN_INFO = {
  name: 0,
  qualified: 1,
  declType: 2,
} as const;

export const COLUMN = {
  encoding: 4,
  width: 5,
  scale: 6,
  nullCount: 8,
  base: 16,
  nulls: 0,
  u8: 1,
  u16: 2,
  u32: 3,
  i64: 4,
  f32: 5,
  f64: 6,
  offsets: 7,
  bytes: 8,
  dictOffsets: 9,
  dictBytes: 10,
  tags: 11,
  blobSizes: 12,
} as const;

export const RESULT_SET = {
  version: 4,
  rowOffset: 8,
  rowCount: 16,
  totalRows: 24,
  changes: 32,
  lastInsertId: 40,
  /** byte 6, bit 0 */
  more: { byte: 6, bit: 0 },
  /** byte 6, bit 1 */
  truncated: { byte: 6, bit: 1 },
  columns: 0,
  data: 1,
} as const;

export enum Encoding {
  Empty = 0,
  Int = 1,
  Real = 2,
  Datetime = 3,
  Text = 4,
  TextDict = 5,
  Blob = 6,
  Mixed = 7,
}

export enum Tag {
  Null = 0,
  Integer = 1,
  Real = 2,
  Datetime = 3,
  Text = 4,
  Blob = 5,
}

export enum Width {
  None = 0,
  U8 = 1,
  U16 = 2,
  U32 = 3,
  I64 = 4,
}

export enum Scale {
  Micro = 0,
  Milli = 1,
  Second = 2,
  Day = 3,
}
