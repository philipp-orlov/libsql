// Written by hand from include/sql/wire/remote.bufs. Re-check against `bufsc --layout` whenever the schema
// changes: the golden test (tools/bufs_golden --remote) fails if the C++ side and these offsets disagree.
//
// Offsets are bytes from the start of a table (its u16 dataBytes, u16 refCount header included); `bit` fields give
// the byte and the bit inside it. Reference slot `i` sits at align4(dataBytes) + 4 * i.

/** Type ids in a message header. */
export const REQUEST_TYPE_ID = 0x9c552ad2;
export const REPLY_TYPE_ID = 0x285ef147;

export const PARAM = { dataBytes: 24, refCount: 2, type: 4, integer: 8, real: 16, name: 0, bytes: 1 } as const;

export const RESULT_OPTIONS = {
  dataBytes: 25,
  refCount: 0,
  batchRows: 4,
  maxRows: 8,
  batchBytes: 16,
  blobPrefix: 20,
  countAllRows: { byte: 24, bit: 0 },
  noDictionary: { byte: 24, bit: 1 },
  noNarrowing: { byte: 24, bit: 2 },
} as const;

export const REQUEST = {
  dataBytes: 52,
  refCount: 6,
  id: 4,
  op: 8,
  readOnly: { byte: 9, bit: 0 },
  what: 10,
  statement: 12,
  txn: 16,
  target: 20,
  credit: 24,
  protocol: 32,
  maxMessage: 36,
  window: 40,
  hints: 48,
  sql: 0,
  params: 1,
  options: 2,
  name: 3,
  client: 4,
  token: 5,
} as const;

export const REPLY = {
  dataBytes: 96,
  refCount: 5,
  id: 4,
  kind: 8,
  constraint: 9,
  code: 10,
  inTransaction: { byte: 12, bit: 0 },
  writes: { byte: 12, bit: 1 },
  readOnly: { byte: 12, bit: 2 },
  strict: { byte: 12, bit: 3 },
  withoutRowid: { byte: 12, bit: 4 },
  protocol: 14,
  changes: 16,
  lastInsertId: 24,
  totalChanges: 32,
  elapsedUs: 40,
  parameters: 48,
  maxMessage: 52,
  session: 56,
  window: 64,
  maxPipeline: 72,
  handleChanges: 80,
  handleLastInsertId: 88,
  message: 0,
  parameterNames: 1,
  columns: 2,
  server: 3,
  sql: 4,
} as const;

export enum Op {
  None = 0,
  Hello = 1,
  Exec = 2,
  Prepare = 3,
  ExecPrepared = 4,
  ClosePrepared = 5,
  Begin = 6,
  Commit = 7,
  Rollback = 8,
  Interrupt = 9,
  Credit = 10,
  Describe = 11,
  Ping = 12,
  State = 13,
  Goodbye = 14,
}

export enum Kind {
  None = 0,
  Welcome = 1,
  Rows = 2,
  Done = 3,
  Error = 4,
  Prepared = 5,
  Pong = 6,
  Closing = 7,
}

export enum Catalog {
  Tables = 0,
  Table = 1,
  Indexes = 2,
}

export enum ParamType {
  Null = 0,
  Integer = 1,
  Real = 2,
  Datetime = 3,
  Text = 4,
  Blob = 5,
}

/** sql::ErrorCode values the UI cares about. */
export enum ErrorCode {
  Ok = 0,
  BadTransaction = 12,
  ReadOnly = 13,
  Internal = 14,
  Runtime = 15,
  Busy = 16,
  Interrupted = 17,
  Connection = 18,
  Protocol = 19,
  PermissionDenied = 20,
}

/** Hello/Exec hint bit 0: the Execute SQL tab's "pending until Write Changes" behaviour. */
export const HINT_MANAGED = 1;
export const PROTOCOL_VERSION = 1;
/** A Rows frame counts at least this much against the flow-control window. */
export const MIN_FRAME_COST = 8192;
