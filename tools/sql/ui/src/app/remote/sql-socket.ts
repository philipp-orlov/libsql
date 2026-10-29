// The browser's end of sql::remote::SqlSocket: a librpc connection (one WebSocket to the server's /api/v1/ws/sql route)
// speaking the Sql service of include/sql/wire/remote.bufs, one request at a time, rows decoded by the same
// ResultSetView as everything else that speaks libbufs. The messages and the method ids come from `rpcc --ts`
// (./wire); flow control, pings, cancellation and framing are the librpc runtime's (./librpc).
// Free of Angular imports on purpose.

import { ResultSetView } from '../bufs';
import { Code, RpcClient, RpcError, RpcStream, WebSocketConstructor } from './librpc/index';
import {
  Catalog,
  ParamListInit,
  ParamType,
  ResultOptionsInit,
  SqlDescribeRequest,
  SqlExecRequest,
  SqlMethods,
  Opened,
  SqlOpenRequest,
  StatementEnd,
} from './wire/remote.bufs';

export { Catalog, ParamType };

/** The ErrorCode values of include/sql/error.hpp that the front end tells apart. */
export enum ErrorCode {
  Ok = 0,
  SyntaxError,
  Unsupported,
  InvalidArgument,
  NoSuchTable,
  TableExists,
  NoSuchColumn,
  AmbiguousColumn,
  NoSuchIndex,
  IndexExists,
  TypeMismatch,
  ConstraintViolation,
  BadTransaction,
  ReadOnly,
  Internal,
  Runtime,
  Busy,
  Interrupted,
  Connection,
  Protocol,
  PermissionDenied,
}

/** `Request.hints` bit 0: the server's ExecInterceptor applies (the Execute SQL tab's "pending until written"). */
const HINT_MANAGED = 1;

/** A parameter value as the protocol carries it. */
export type ParamValue =
  | null
  | { type: ParamType.Integer | ParamType.Datetime; value: bigint | number }
  | { type: ParamType.Real; value: number }
  | { type: ParamType.Text; value: string }
  | { type: ParamType.Blob; value: Uint8Array };

export interface ResultOptionsFields {
  maxRows?: bigint | number;
  batchRows?: number;
  batchBytes?: number;
  blobPrefix?: number;
  countAllRows?: boolean;
  noDictionary?: boolean;
  noNarrowing?: boolean;
}

export class SocketError extends Error {
  constructor(
    message: string,
    readonly code: number = ErrorCode.Connection,
  ) {
    super(message);
    this.name = 'SocketError';
  }
}

/** How a statement ended (the stream's trailer), and what the server said when it opened the session. */
export interface Reply {
  changes: number;
  lastInsertId: number;
  totalChanges: number;
  inTransaction: boolean;
  elapsedUs: number;
  sql: string;
  strict: boolean;
  withoutRowid: boolean;
  handleChanges: number;
  handleLastInsertId: number;
  /** Open only. */
  server: string;
  session: number;
  readOnly: boolean;
}

export interface SqlSocketOptions {
  /** Sent in the first message when the server asks for a token. */
  token?: string;
  /** Bytes of rows the server may send before this reads them. Default 1 MiB. */
  window?: number;
  /** A ping request this often, while connected, so an idle timeout on the way does not cut the connection. */
  pingMs?: number;
  /** For tests: the WebSocket constructor to use instead of the browser's. */
  WebSocket?: WebSocketConstructor;
}

/** What a finished request produced. */
export interface SocketResult {
  /** The rows (and the statement's columns); null only when the server sent no rows. */
  view: ResultSetView | null;
  done: Reply;
}

const emptyReply = (): Reply => ({
  changes: 0,
  lastInsertId: 0,
  totalChanges: 0,
  inTransaction: false,
  elapsedUs: 0,
  sql: '',
  strict: false,
  withoutRowid: false,
  handleChanges: 0,
  handleLastInsertId: 0,
  server: '',
  session: 0,
  readOnly: false,
});

/** The error a failed call stands for: the server's own code travels in the status detail. */
function socketError(e: unknown): SocketError {
  if (e instanceof SocketError) {
    return e;
  }
  if (e instanceof RpcError) {
    if ((e.detail & 0x80000000) !== 0) {
      return new SocketError(e.message.replace(/^[A-Za-z]+: /, ''), (e.detail >>> 8) & 0xffff);
    }
    switch (e.code) {
      case Code.Cancelled:
        return new SocketError('interrupted', ErrorCode.Interrupted);
      case Code.PermissionDenied:
      case Code.Unauthenticated:
        return new SocketError(e.message, ErrorCode.PermissionDenied);
      case Code.DataLoss:
        return new SocketError(e.message, ErrorCode.Protocol);
      default:
        return new SocketError(e.message, ErrorCode.Connection);
    }
  }
  return new SocketError(e instanceof Error ? e.message : String(e));
}

function encodeParams(params: ParamValue[] | undefined): ParamListInit {
  return {
    list: (params ?? []).map(p => {
      if (p === null) {
        return {};
      }
      switch (p.type) {
        case ParamType.Real:
          return { type: p.type, real: p.value };
        case ParamType.Text:
          return { type: p.type, bytes: new TextEncoder().encode(p.value) };
        case ParamType.Blob:
          return { type: p.type, bytes: p.value };
        default:
          return { type: p.type, integer: p.value };
      }
    }),
  };
}

export class SqlSocket {
  private rpc: RpcClient | null = null;
  private running: RpcStream<Uint8Array, StatementEnd> | null = null;
  private chain: Promise<unknown> = Promise.resolve();
  private inTransaction = false;
  private welcomed: Reply | null = null;

  constructor(
    private readonly url: string,
    private readonly options: SqlSocketOptions = {},
  ) {}

  get connected(): boolean {
    return this.rpc !== null && this.rpc.connected;
  }

  /** What the server said when it opened the session (its name, whether it is read-only); null before that. */
  get welcome(): Reply | null {
    return this.welcomed;
  }

  /** Opens the connection and the session; resolves when the server has welcomed this client. */
  async connect(): Promise<void> {
    try {
      this.rpc = await RpcClient.connect(this.url, {
        token: this.options.token,
        window: this.options.window,
        pingIntervalMs: this.options.pingMs,
        WebSocket: this.options.WebSocket,
        peer: 'sql-ui/1.0',
      });
      const opened: Opened = await this.rpc.call(SqlMethods.Open, SqlOpenRequest.encode({ readOnly: false, client: 'sql-ui/1.0' }), Opened.decode);
      this.welcomed = { ...emptyReply(), server: opened.server, session: Number(opened.session), readOnly: opened.readOnly };
    } catch (e) {
      this.rpc?.close();
      this.rpc = null;
      throw socketError(e);
    }
  }

  /**
   * Runs `sql` and resolves with its rows and counters. One request runs at a time: calls queue. A connection the
   * server closed while idle is replaced, unless a transaction was open on it.
   */
  exec(sql: string, params?: ParamValue[], options?: ResultOptionsFields): Promise<SocketResult> {
    return this.queue(() => ({
      method: SqlMethods.Exec,
      request: SqlExecRequest.encode({ text: sql, params: encodeParams(params), options: options as ResultOptionsInit, hints: HINT_MANAGED }),
    }));
  }

  /**
   * The catalog: the table names, one table's columns (the trailer carries its CREATE statement, strict and
   * without_rowid), or the columns of the indexes of `name` (all tables when empty). The rows are those of the
   * server's Describe.
   */
  describe(what: Catalog, name = ''): Promise<SocketResult> {
    return this.queue(() => ({ method: SqlMethods.Describe, request: SqlDescribeRequest.encode({ what, name }) }));
  }

  private queue(build: () => { method: number; request: Uint8Array }): Promise<SocketResult> {
    const run = () => this.run(build());
    const result = this.chain.then(run, run);
    this.chain = result.catch(() => undefined);
    return result;
  }

  /** Asks the server to stop the running request, which then ends with ErrorCode.Interrupted. */
  interrupt(): void {
    this.running?.cancel();
  }

  close(): void {
    this.rpc?.close();
    this.rpc = null;
  }

  // ---------------------------------------------------------------------------------------------

  private async run(call: { method: number; request: Uint8Array }): Promise<SocketResult> {
    if (!this.connected) {
      if (this.inTransaction) {
        this.inTransaction = false;
        throw new SocketError('the connection was lost; the transaction was rolled back');
      }
      await this.connect();
    }
    const frames: Uint8Array[] = [];
    const stream = this.rpc!.stream<Uint8Array, StatementEnd>(call.method, call.request, body => body, {}, StatementEnd.decode);
    this.running = stream;
    try {
      for await (const body of stream) {
        frames.push(body);
      }
    } catch (e) {
      this.running = null;
      const error = socketError(e);
      if (!this.connected && this.inTransaction) {
        this.inTransaction = false;
        throw new SocketError('the connection was lost; the transaction was rolled back');
      }
      throw error;
    }
    this.running = null;
    const end = stream.trailer;
    const done: Reply = {
      ...emptyReply(),
      changes: Number(end?.changes ?? 0n),
      lastInsertId: Number(end?.lastInsertId ?? 0n),
      totalChanges: Number(end?.totalChanges ?? 0n),
      inTransaction: end?.inTransaction ?? false,
      elapsedUs: Number(end?.elapsedUs ?? 0n),
      sql: end?.sql ?? '',
      strict: end?.strict ?? false,
      withoutRowid: end?.withoutRowid ?? false,
      handleChanges: Number(end?.handleChanges ?? 0n),
      handleLastInsertId: Number(end?.handleLastInsertId ?? 0n),
    };
    this.inTransaction = done.inTransaction;
    return { view: SqlSocket.view(frames), done };
  }

  /** The batches of one result as one stream: a single batch is read where it lies, several are joined. */
  private static view(frames: Uint8Array[]): ResultSetView | null {
    if (frames.length === 0) {
      return null;
    }
    if (frames.length === 1) {
      const f = frames[0];
      return ResultSetView.open(f.buffer as ArrayBuffer, f.byteOffset, f.byteLength);
    }
    let total = 0;
    for (const f of frames) {
      total += f.byteLength;
    }
    const joined = new Uint8Array(total);
    let at = 0;
    for (const f of frames) {
      joined.set(f, at);
      at += f.byteLength;
    }
    return ResultSetView.open(joined.buffer);
  }
}
