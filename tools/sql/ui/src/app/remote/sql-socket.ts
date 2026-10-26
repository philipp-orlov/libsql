// The browser's end of sql::remote::SqlSocket (sql-remote.md): one WebSocket to the server's /api/v1/ws/sql route,
// one request at a time, rows decoded by the same ResultSetView as everything else that speaks libbufs.
//
// Flow control, as the C++ client does it: the server sends at most a window of rows before it hears back, so after
// reading half a window this sends a Credit request; whatever is left over goes ahead of the next request.
// Free of Angular imports on purpose.

import { ResultSetView } from '../bufs';
import { ErrorCode, HINT_MANAGED, Kind, MIN_FRAME_COST, Op, PROTOCOL_VERSION } from './layout';
import { Reply, decodeReply } from './reply';
import { RequestFields, encodeRequest } from './request';

export class SocketError extends Error {
  constructor(
    message: string,
    readonly code: number = ErrorCode.Connection,
  ) {
    super(message);
    this.name = 'SocketError';
  }
}

/** The part of the browser's WebSocket this uses; tests pass a fake. */
export interface WebSocketLike {
  binaryType: string;
  readyState: number;
  onopen: ((event: unknown) => void) | null;
  onmessage: ((event: { data: unknown }) => void) | null;
  onclose: ((event: { code?: number; reason?: string }) => void) | null;
  onerror: ((event: unknown) => void) | null;
  send(data: ArrayBuffer): void;
  close(code?: number, reason?: string): void;
}

export interface SqlSocketOptions {
  /** Sent in the first message when the server asks for a token. */
  token?: string;
  /** Bytes of rows the server may send before this reads them. Default 1 MiB. */
  window?: number;
  /** A ping request this often, while connected, so an idle timeout on the way does not cut the connection. */
  pingMs?: number;
  /** For tests. */
  createSocket?: (url: string, protocols: string[]) => WebSocketLike;
}

/** What a finished request produced. */
export interface SocketResult {
  /** The rows (and the statement's columns); null only when the server sent no rows frame. */
  view: ResultSetView | null;
  done: Reply;
}

const OPEN = 1;

interface Pending {
  id: number;
  frames: { buffer: ArrayBuffer; offset: number; length: number }[];
  resolve: (result: SocketResult) => void;
  reject: (error: Error) => void;
}

export class SqlSocket {
  private socket: WebSocketLike | null = null;
  private pending: Pending | null = null;
  private hello: { id: number; resolve: (r: Reply) => void; reject: (e: Error) => void } | null = null;
  private chain: Promise<unknown> = Promise.resolve();
  private nextId = 1;
  private window = 1 << 20;
  private consumed = 0;
  private inTransaction = false;
  private pinger: ReturnType<typeof setInterval> | null = null;

  constructor(
    private readonly url: string,
    private readonly options: SqlSocketOptions = {},
  ) {}

  get connected(): boolean {
    return this.socket !== null && this.socket.readyState === OPEN;
  }

  /** Opens the connection and says hello; resolves when the server has welcomed this client. */
  connect(): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const factory = this.options.createSocket ?? ((url: string, protocols: string[]) => new WebSocket(url, protocols) as unknown as WebSocketLike);
      let socket: WebSocketLike;
      try {
        socket = factory(this.url, ['libsql.v1']);
      } catch (e) {
        reject(new SocketError(`cannot open ${this.url}: ${e instanceof Error ? e.message : String(e)}`));
        return;
      }
      socket.binaryType = 'arraybuffer';
      this.socket = socket;
      this.consumed = 0;
      socket.onopen = () => {
        const id = this.take();
        this.hello = {
          id,
          resolve: welcome => {
            this.window = welcome.window || this.window;
            this.startPings();
            resolve();
          },
          reject,
        };
        socket.send(
          encodeRequest({
            id,
            op: Op.Hello,
            protocol: PROTOCOL_VERSION,
            client: 'sql-ui/1.0',
            token: this.options.token,
            window: this.options.window ?? 1 << 20,
            maxMessage: 16 << 20,
          }),
        );
      };
      socket.onmessage = event => this.onMessage(event.data);
      socket.onerror = () => undefined;
      socket.onclose = event => {
        const wasOurs = this.socket === socket;
        if (wasOurs) {
          this.socket = null;
        }
        this.stopPings();
        const error = new SocketError(`the connection was closed${event.reason ? `: ${event.reason}` : ''}${event.code ? ` (${event.code})` : ''}`);
        this.hello?.reject(error);
        this.hello = null;
        if (this.pending) {
          const lost = this.inTransaction ? new SocketError('the connection was lost; the transaction was rolled back') : error;
          this.inTransaction = false;
          this.pending.reject(lost);
          this.pending = null;
        }
      };
    });
  }

  /**
   * Runs `sql` and resolves with its rows and counters. One request runs at a time: calls queue. A connection the
   * server closed while idle is replaced, unless a transaction was open on it.
   */
  exec(sql: string, params?: RequestFields['params']): Promise<SocketResult> {
    const run = () => this.run({ sql, params, hints: HINT_MANAGED });
    const result = this.chain.then(run, run);
    this.chain = result.catch(() => undefined);
    return result;
  }

  /** Asks the server to stop the running request, which then ends with ErrorCode.Interrupted. */
  interrupt(): void {
    if (this.pending && this.connected) {
      this.socket!.send(encodeRequest({ id: this.take(), op: Op.Interrupt, target: this.pending.id }));
    }
  }

  close(): void {
    this.stopPings();
    const socket = this.socket;
    this.socket = null;
    if (socket && socket.readyState === OPEN) {
      socket.send(encodeRequest({ id: this.take(), op: Op.Goodbye }));
    }
    socket?.close(1000, 'goodbye');
  }

  // ---------------------------------------------------------------------------------------------

  private take(): number {
    const id = this.nextId++;
    if (this.nextId >= 0x80000000) {
      this.nextId = 1;
    }
    return id;
  }

  private async run(fields: { sql: string; params?: RequestFields['params']; hints: number }): Promise<SocketResult> {
    if (!this.connected) {
      if (this.inTransaction) {
        this.inTransaction = false;
        throw new SocketError('the connection was lost; the transaction was rolled back');
      }
      await this.connect();
    }
    if (this.consumed > 0) {
      this.sendCredit();
    }
    const id = this.take();
    return new Promise<SocketResult>((resolve, reject) => {
      this.pending = { id, frames: [], resolve, reject };
      this.socket!.send(encodeRequest({ id, op: Op.Exec, sql: fields.sql, params: fields.params, hints: fields.hints }));
    });
  }

  private sendCredit(): void {
    const credit = this.consumed;
    this.consumed = 0;
    this.socket!.send(encodeRequest({ id: this.take(), op: Op.Credit, credit }));
  }

  private onMessage(data: unknown): void {
    if (!(data instanceof ArrayBuffer)) {
      return;
    }
    let reply: Reply;
    try {
      reply = decodeReply(data);
    } catch {
      this.fail(new SocketError('the server sent a message that is not a reply', ErrorCode.Protocol));
      return;
    }
    if (reply.kind === Kind.Closing) {
      return; // the close that follows tells the caller
    }
    if (this.hello && reply.id === this.hello.id) {
      const hello = this.hello;
      this.hello = null;
      if (reply.kind === Kind.Welcome) {
        hello.resolve(reply);
      } else {
        hello.reject(new SocketError(reply.message || 'the server refused the connection', reply.code));
      }
      return;
    }
    const pending = this.pending;
    if (!pending || reply.id !== pending.id) {
      return; // a pong, or the answer of something nobody waits for
    }
    if (reply.kind === Kind.Rows) {
      this.consumed += Math.max(data.byteLength, MIN_FRAME_COST);
      pending.frames.push({ buffer: data, offset: reply.size, length: data.byteLength - reply.size });
      if (this.consumed >= this.window / 2) {
        this.sendCredit();
      }
      return;
    }
    this.pending = null;
    if (reply.kind === Kind.Error) {
      pending.reject(new SocketError(reply.message, reply.code));
      return;
    }
    this.inTransaction = reply.inTransaction;
    pending.resolve({ view: SqlSocket.view(pending.frames), done: reply });
  }

  /** The batches of one result as one stream: a single batch is read where it lies, several are joined. */
  private static view(frames: Pending['frames']): ResultSetView | null {
    if (frames.length === 0) {
      return null;
    }
    if (frames.length === 1) {
      return ResultSetView.open(frames[0].buffer, frames[0].offset, frames[0].length);
    }
    let total = 0;
    for (const f of frames) {
      total += f.length;
    }
    const joined = new Uint8Array(total);
    let at = 0;
    for (const f of frames) {
      joined.set(new Uint8Array(f.buffer, f.offset, f.length), at);
      at += f.length;
    }
    return ResultSetView.open(joined.buffer);
  }

  private fail(error: Error): void {
    this.pending?.reject(error);
    this.pending = null;
    this.hello?.reject(error);
    this.hello = null;
  }

  private startPings(): void {
    this.stopPings();
    const every = this.options.pingMs ?? 20000;
    if (every > 0) {
      this.pinger = setInterval(() => {
        if (this.connected) {
          this.socket!.send(encodeRequest({ id: this.take(), op: Op.Ping }));
        }
      }, every);
    }
  }

  private stopPings(): void {
    if (this.pinger !== null) {
      clearInterval(this.pinger);
      this.pinger = null;
    }
  }
}
