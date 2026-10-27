import { Injectable, signal } from '@angular/core';

import { BrowseReply, BrowseRequest, Cell, DatabaseInfo, PragmaInfo, Schema, SqlResult } from '../models/api.models';
import { Catalog, ErrorCode, ParamValue, Reply, ResultOptionsFields, SocketError, SocketResult, SqlSocket } from '../remote';
import { fromBufs } from '../utils/sql-result';
import * as catalog from './catalog';

/** Where the server's SQL route is (sql::remote::SqlSocket; `sql --listen` serves it here). */
export const SOCKET_PATH = '/api/v1/ws/sql';

export class ApiError extends Error {
  constructor(
    message: string,
    /** 0: the server cannot be reached; 401: it wants a token; 400: the request was refused. */
    readonly status: number,
  ) {
    super(message);
  }
}

const EMPTY_SCHEMA: Schema = { tables: [], views: [], indexes: [], triggers: [] };

/** The part of SqlSocket the service uses; tests give it a fake. */
export interface QuerySocket {
  readonly connected: boolean;
  readonly welcome: Reply | null;
  connect(): Promise<void>;
  exec(sql: string, params?: ParamValue[], options?: ResultOptionsFields): Promise<SocketResult>;
  describe(what: Catalog, name?: string): Promise<SocketResult>;
  interrupt(): void;
  close(): void;
}

/**
 * The server's database: what the tabs show, and every call that reads or changes it. All of it goes over the one
 * WebSocket to the server's SQL route; the SQL the tabs need is written in catalog.ts.
 */
@Injectable({ providedIn: 'root' })
export class DatabaseService {
  readonly info = signal<DatabaseInfo | null>(null);
  readonly schema = signal<Schema>(EMPTY_SCHEMA);
  /** True while changes wait for Write Changes. */
  readonly inTransaction = signal(false);
  /** Grows when the data or the structure may have changed, so that open views reload. */
  readonly revision = signal(0);
  /** True when the server refused the connection for want of a (correct) token: the shell asks for one. */
  readonly authRequired = signal(false);

  /** Where queries go; replaced in tests. */
  socketFactory: (url: string, token: string | undefined) => QuerySocket = (url, token) => new SqlSocket(url, { token });
  private socket: QuerySocket | null = null;

  /** The connection the catalog functions (and the Visualize tab's) ask through. */
  readonly conn: catalog.Conn = {
    exec: (sql, params, options) => this.call(socket => socket.exec(sql, params, options)),
    describe: (what, name) => this.call(socket => socket.describe(what, name)),
  };

  private get readOnly(): boolean {
    return this.info()?.readOnly ?? false;
  }

  async load(): Promise<void> {
    await this.open();
    const welcome = this.socket!.welcome;
    const facts = await catalog.loadFacts(this.conn);
    this.info.set({
      path: facts.path,
      version: (welcome?.server ?? '').split('/')[1] ?? welcome?.server ?? '',
      readOnly: welcome?.readOnly ?? false,
      functions: facts.functions,
      inTransaction: this.inTransaction(),
    });
    await this.loadSchema();
  }

  async loadSchema(): Promise<void> {
    this.schema.set(await this.run(() => catalog.loadSchema(this.conn)));
  }

  /** A script that rebuilds the schema and the rows. */
  dump(): Promise<string> {
    return this.run(() => catalog.dump(this.conn));
  }

  async browse(request: BrowseRequest): Promise<BrowseReply> {
    const page = await this.run(() => catalog.browse(this.conn, request, this.readOnly));
    return { ...page, inTransaction: this.inTransaction() };
  }

  async insertRow(table: string, values: Record<string, Cell>): Promise<void> {
    await this.run(() => catalog.insertRow(this.conn, table, values, this.readOnly));
    this.changed();
  }

  async updateRow(table: string, key: Cell[], set: Record<string, Cell>): Promise<void> {
    await this.run(() => catalog.updateRow(this.conn, table, key, set, this.readOnly));
    this.changed();
  }

  async deleteRows(table: string, keys: Cell[][]): Promise<void> {
    await this.run(() => catalog.deleteRows(this.conn, table, keys, this.readOnly));
    this.changed();
  }

  async execute(sql: string): Promise<SqlResult> {
    const wasPending = this.inTransaction();
    const { view, done } = await this.call(socket => socket.exec(sql, undefined, catalog.RESULT_OPTIONS));
    const result = fromBufs(view, done.elapsedUs / 1000, done.inTransaction);
    if (!view) {
      // A statement without rows reports its counters in the Done.
      result.changes = done.changes;
      result.lastInsertId = done.lastInsertId;
    }
    // A run that left nothing pending changed nothing.
    if (wasPending || result.inTransaction) {
      this.changed();
      await this.loadSchema();
    }
    return result;
  }

  /** Stops the statement the Execute SQL tab is waiting for; it then fails as interrupted. */
  interruptRunning(): void {
    this.socket?.interrupt();
  }

  async commit(): Promise<void> {
    if (this.inTransaction()) {
      await this.conn.exec('COMMIT');
    }
    this.changed();
    await this.loadSchema();
  }

  async revert(): Promise<void> {
    if (this.inTransaction()) {
      await this.conn.exec('ROLLBACK');
    }
    this.changed();
    await this.loadSchema();
  }

  async pragmas(): Promise<{ pragmas: PragmaInfo[]; inTransaction: boolean }> {
    const pragmas = await this.run(() => catalog.loadPragmas(this.conn, this.readOnly));
    return { pragmas, inTransaction: this.inTransaction() };
  }

  async setPragma(name: string, value: number): Promise<void> {
    await this.run(() => catalog.setPragma(this.conn, name, value, this.readOnly, this.inTransaction()));
    this.changed();
  }

  /** Connects again with this token, for a server that asked for one. */
  async useToken(token: string): Promise<void> {
    try {
      sessionStorage.setItem('sql.token', token);
    } catch {
      // the token then lasts for this page only
    }
    this.pageToken = token;
    this.socket?.close();
    this.socket = null;
    this.authRequired.set(false);
    await this.load();
  }

  // -------------------------------------------------------------------------------------------------

  private pageToken: string | null = null;

  private changed(): void {
    this.revision.update(count => count + 1);
  }

  private storedToken(): string | null {
    if (this.pageToken) {
      return this.pageToken;
    }
    try {
      return sessionStorage.getItem('sql.token');
    } catch {
      return null;
    }
  }

  /** The connected socket; callers that arrive while it is being opened wait for that one connection. */
  private open(): Promise<QuerySocket> {
    if (this.socket?.connected) {
      return Promise.resolve(this.socket);
    }
    this.opening ??= this.connectSocket().finally(() => (this.opening = null));
    return this.opening;
  }

  private opening: Promise<QuerySocket> | null = null;

  private async connectSocket(): Promise<QuerySocket> {
    if (!this.socket) {
      this.socket = this.socketFactory(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}${SOCKET_PATH}`, this.storedToken() ?? undefined);
    }
    try {
      await this.socket.connect();
    } catch (failure) {
      this.socket = null;
      throw this.asApiError(failure);
    }
    return this.socket;
  }

  /** One request over the socket; its Done says whether changes are pending. */
  private async call(request: (socket: QuerySocket) => Promise<SocketResult>): Promise<SocketResult> {
    const socket = await this.open();
    try {
      const result = await request(socket);
      this.inTransaction.set(result.done.inTransaction);
      return result;
    } catch (failure) {
      if (failure instanceof SocketError && failure.code === ErrorCode.Connection) {
        this.inTransaction.set(false); // the server rolled it back with the connection
      }
      throw this.asApiError(failure);
    }
  }

  /** Runs a catalog function, turning what it throws into an ApiError. */
  private async run<T>(work: () => Promise<T>): Promise<T> {
    try {
      return await work();
    } catch (failure) {
      throw this.asApiError(failure);
    }
  }

  private asApiError(failure: unknown): Error {
    if (failure instanceof ApiError) {
      return failure;
    }
    if (failure instanceof SocketError) {
      if (failure.code === ErrorCode.PermissionDenied) {
        this.authRequired.set(true);
        return new ApiError(failure.message || 'The server wants a token.', 401);
      }
      return new ApiError(failure.message, failure.code === ErrorCode.Connection ? 0 : 400);
    }
    if (failure instanceof catalog.CatalogError) {
      return new ApiError(failure.message, 400);
    }
    return failure instanceof Error ? failure : new Error(String(failure));
  }
}
