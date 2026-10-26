// Encoding the protocol's Request (include/sql/wire/remote.bufs). The order of the builder calls is the one of
// buildRequest() in src/remote/protocol.cpp, which fixes the message's layout: id, op and every scalar, then the
// strings and tables sql, params, options, name, client, token. The golden test compares the bytes.

import { MessageBuilder } from './builder';
import { Catalog, Op, PARAM, ParamType, REQUEST, REQUEST_TYPE_ID, RESULT_OPTIONS } from './layout';

/** A parameter value as the protocol carries it. */
export type ParamValue =
  | null
  | { type: ParamType.Integer | ParamType.Datetime; value: bigint | number }
  | { type: ParamType.Real; value: number }
  | { type: ParamType.Text; value: string }
  | { type: ParamType.Blob; value: Uint8Array };

export interface NamedParam {
  name: string;
  value: ParamValue;
}

export interface ResultOptionsFields {
  maxRows?: bigint | number;
  batchRows?: number;
  batchBytes?: number;
  blobPrefix?: number;
  countAllRows?: boolean;
  noDictionary?: boolean;
  noNarrowing?: boolean;
}

export interface RequestFields {
  id: number;
  op: Op;
  sql?: string;
  statement?: number;
  txn?: number;
  target?: number;
  credit?: bigint | number;
  readOnly?: boolean;
  what?: Catalog;
  name?: string;
  protocol?: number;
  client?: string;
  token?: string;
  window?: bigint | number;
  maxMessage?: number;
  hints?: number;
  /** Positional parameters. */
  params?: ParamValue[];
  /** Named parameters; they win over `params`. */
  named?: NamedParam[];
  /** Written only when given. */
  options?: ResultOptionsFields;
}

function encodeValue(mb: MessageBuilder, param: number, value: ParamValue): void {
  if (value === null) {
    return;
  }
  mb.u8(param + PARAM.type, value.type);
  switch (value.type) {
    case ParamType.Integer:
    case ParamType.Datetime:
      mb.i64(param + PARAM.integer, value.value);
      break;
    case ParamType.Real:
      mb.f64(param + PARAM.real, value.value);
      break;
    case ParamType.Text:
      mb.setRef(param, PARAM.dataBytes, PARAM.bytes, mb.byteVector(new TextEncoder().encode(value.value)));
      break;
    case ParamType.Blob:
      mb.setRef(param, PARAM.dataBytes, PARAM.bytes, mb.byteVector(value.value));
      break;
  }
}

export function encodeRequest(f: RequestFields): ArrayBuffer {
  const mb = new MessageBuilder(REQUEST_TYPE_ID, REQUEST.dataBytes, REQUEST.refCount);
  const root = 8;
  mb.u32(root + REQUEST.id, f.id);
  mb.u8(root + REQUEST.op, f.op);
  mb.u32(root + REQUEST.statement, f.statement ?? 0);
  mb.u32(root + REQUEST.txn, f.txn ?? 0);
  mb.u32(root + REQUEST.target, f.target ?? 0);
  mb.u64(root + REQUEST.credit, f.credit ?? 0);
  mb.bit(root + REQUEST.readOnly.byte, REQUEST.readOnly.bit, f.readOnly ?? false);
  mb.u8(root + REQUEST.what, f.what ?? Catalog.Tables);
  mb.u16(root + REQUEST.protocol, f.protocol ?? 0);
  mb.u64(root + REQUEST.window, f.window ?? 0);
  mb.u32(root + REQUEST.maxMessage, f.maxMessage ?? 0);
  mb.u32(root + REQUEST.hints, f.hints ?? 0);
  if (f.sql) {
    mb.setRef(root, REQUEST.dataBytes, REQUEST.sql, mb.string(f.sql));
  }
  const count = f.named ? f.named.length : (f.params?.length ?? 0);
  if (count > 0) {
    const list = mb.vector(count, 4, 4);
    mb.setRef(root, REQUEST.dataBytes, REQUEST.params, list);
    for (let i = 0; i < count; i++) {
      const param = mb.table(PARAM.dataBytes, PARAM.refCount);
      mb.setElementRef(list, i, param);
      if (f.named) {
        mb.setRef(param, PARAM.dataBytes, PARAM.name, mb.string(f.named[i].name));
        encodeValue(mb, param, f.named[i].value);
      } else {
        encodeValue(mb, param, f.params![i]);
      }
    }
  }
  if (f.options) {
    const o = f.options;
    const table = mb.table(RESULT_OPTIONS.dataBytes, RESULT_OPTIONS.refCount);
    mb.setRef(root, REQUEST.dataBytes, REQUEST.options, table);
    mb.u64(table + RESULT_OPTIONS.maxRows, o.maxRows ?? 0);
    mb.u32(table + RESULT_OPTIONS.batchRows, o.batchRows ?? 0);
    mb.u32(table + RESULT_OPTIONS.batchBytes, o.batchBytes ?? 0);
    mb.u32(table + RESULT_OPTIONS.blobPrefix, o.blobPrefix ?? 0);
    mb.bit(table + RESULT_OPTIONS.countAllRows.byte, RESULT_OPTIONS.countAllRows.bit, o.countAllRows ?? false);
    mb.bit(table + RESULT_OPTIONS.noDictionary.byte, RESULT_OPTIONS.noDictionary.bit, o.noDictionary ?? false);
    mb.bit(table + RESULT_OPTIONS.noNarrowing.byte, RESULT_OPTIONS.noNarrowing.bit, o.noNarrowing ?? false);
  }
  if (f.name) {
    mb.setRef(root, REQUEST.dataBytes, REQUEST.name, mb.string(f.name));
  }
  if (f.client) {
    mb.setRef(root, REQUEST.dataBytes, REQUEST.client, mb.string(f.client));
  }
  if (f.token) {
    mb.setRef(root, REQUEST.dataBytes, REQUEST.token, mb.string(f.token));
  }
  return mb.finish();
}
