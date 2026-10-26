// Decoding the protocol's Reply (include/sql/wire/remote.bufs). The browser trusts its own server, and a bad
// offset makes DataView throw a RangeError rather than read wild memory, so there is no verifier.

import { Table } from '../bufs/message';
import { Kind, REPLY, REPLY_TYPE_ID } from './layout';

export interface Reply {
  id: number;
  kind: Kind;
  code: number;
  constraint: number;
  message: string;
  changes: number;
  lastInsertId: number;
  totalChanges: number;
  inTransaction: boolean;
  elapsedUs: number;
  parameters: number;
  parameterNames: string[];
  columns: string[];
  writes: boolean;
  protocol: number;
  server: string;
  session: number;
  readOnly: boolean;
  window: number;
  maxMessage: number;
  maxPipeline: number;
  sql: string;
  strict: boolean;
  withoutRowid: boolean;
  handleChanges: number;
  handleLastInsertId: number;
  /** Size of the Reply message; a ResultSet message follows at this offset in a Rows frame. */
  size: number;
}

export class ReplyError extends Error {
  constructor(message = 'not a libsql reply') {
    super(message);
    this.name = 'ReplyError';
  }
}

/** Reads the Reply at the start of a frame. */
export function decodeReply(buffer: ArrayBuffer, byteOffset = 0): Reply {
  const view = new DataView(buffer, byteOffset);
  if (view.byteLength < 8 || view.getUint32(4, true) !== REPLY_TYPE_ID) {
    throw new ReplyError();
  }
  const size = view.getUint32(0, true);
  if (size < 8 || size % 8 !== 0 || size > view.byteLength) {
    throw new ReplyError();
  }
  const t = new Table(view, 8);
  const strings = (index: number): string[] => {
    const vector = t.vector(index);
    const out: string[] = [];
    for (let i = 0; vector && i < vector.count; i++) {
      out.push(t.stringAt(vector, i));
    }
    return out;
  };
  return {
    id: t.u32(REPLY.id),
    kind: t.u8(REPLY.kind) as Kind,
    code: t.u16(REPLY.code),
    constraint: t.u8(REPLY.constraint),
    message: t.string(REPLY.message),
    changes: Number(t.u64(REPLY.changes)),
    lastInsertId: Number(t.i64(REPLY.lastInsertId)),
    totalChanges: Number(t.i64(REPLY.totalChanges)),
    inTransaction: t.bit(REPLY.inTransaction.byte, REPLY.inTransaction.bit),
    elapsedUs: Number(t.u64(REPLY.elapsedUs)),
    parameters: t.u32(REPLY.parameters),
    parameterNames: strings(REPLY.parameterNames),
    columns: strings(REPLY.columns),
    writes: t.bit(REPLY.writes.byte, REPLY.writes.bit),
    protocol: t.u16(REPLY.protocol),
    server: t.string(REPLY.server),
    session: Number(t.u64(REPLY.session)),
    readOnly: t.bit(REPLY.readOnly.byte, REPLY.readOnly.bit),
    window: Number(t.u64(REPLY.window)),
    maxMessage: t.u32(REPLY.maxMessage),
    maxPipeline: t.u16(REPLY.maxPipeline),
    sql: t.string(REPLY.sql),
    strict: t.bit(REPLY.strict.byte, REPLY.strict.bit),
    withoutRowid: t.bit(REPLY.withoutRowid.byte, REPLY.withoutRowid.bit),
    handleChanges: Number(t.i64(REPLY.handleChanges)),
    handleLastInsertId: Number(t.i64(REPLY.handleLastInsertId)),
    size,
  };
}
