import { HttpClient, HttpErrorResponse } from '@angular/common/http';
import { Injectable, inject, signal } from '@angular/core';
import { Observable, firstValueFrom } from 'rxjs';

import {
  BrowseReply,
  BrowseRequest,
  Cell,
  ChangeReply,
  DatabaseInfo,
  DumpReply,
  Pending,
  PragmasReply,
  Schema,
  SchemaReply,
  SqlReply,
} from '../models/api.models';

export class ApiError extends Error {
  constructor(
    message: string,
    readonly status: number,
  ) {
    super(message);
  }
}

function describeFailure(failure: HttpErrorResponse): string {
  const body = failure.error as { error?: unknown } | null;
  if (body && typeof body.error === 'string') {
    return body.error;
  }
  return failure.status === 0 ? 'The server cannot be reached.' : failure.message;
}

const EMPTY_SCHEMA: Schema = { tables: [], views: [], indexes: [], triggers: [] };

/** The server's database: what the tabs show, and every call that reads or changes it. */
@Injectable({ providedIn: 'root' })
export class DatabaseService {
  private readonly http = inject(HttpClient);

  readonly info = signal<DatabaseInfo | null>(null);
  readonly schema = signal<Schema>(EMPTY_SCHEMA);
  /** True while changes wait for Write Changes. */
  readonly inTransaction = signal(false);
  /** Grows when the data or the structure may have changed, so that open views reload. */
  readonly revision = signal(0);

  async load(): Promise<void> {
    this.info.set(await this.get<DatabaseInfo>('/api/info'));
    await this.loadSchema();
  }

  async loadSchema(): Promise<void> {
    const { tables, views, indexes, triggers } = await this.get<SchemaReply>('/api/schema');
    this.schema.set({ tables, views, indexes, triggers });
  }

  /** A script that rebuilds the schema and the rows. */
  async dump(): Promise<string> {
    return (await this.get<DumpReply>('/api/dump')).sql;
  }

  browse(request: BrowseRequest): Promise<BrowseReply> {
    return this.post<BrowseReply>('/api/browse', request);
  }

  async insertRow(table: string, values: Record<string, Cell>): Promise<void> {
    await this.post<ChangeReply>('/api/rows/insert', { table, values });
    this.changed();
  }

  async updateRow(table: string, key: Cell[], set: Record<string, Cell>): Promise<void> {
    await this.post<ChangeReply>('/api/rows/update', { table, key, set });
    this.changed();
  }

  async deleteRows(table: string, keys: Cell[][]): Promise<void> {
    await this.post<ChangeReply>('/api/rows/delete', { table, keys });
    this.changed();
  }

  async execute(sql: string): Promise<SqlReply> {
    const wasPending = this.inTransaction();
    const reply = await this.post<SqlReply>('/api/sql', { sql });
    // A run that left nothing pending changed nothing.
    if (wasPending || reply.inTransaction) {
      this.changed();
      await this.loadSchema();
    }
    return reply;
  }

  async commit(): Promise<void> {
    await this.post<Pending>('/api/commit', {});
    this.changed();
    await this.loadSchema();
  }

  async revert(): Promise<void> {
    await this.post<Pending>('/api/revert', {});
    this.changed();
    await this.loadSchema();
  }

  pragmas(): Promise<PragmasReply> {
    return this.get<PragmasReply>('/api/pragmas');
  }

  async setPragma(name: string, value: number): Promise<void> {
    await this.post<Pending>('/api/pragmas', { name, value });
    this.changed();
  }

  private changed(): void {
    this.revision.update(count => count + 1);
  }

  private get<T extends Pending>(url: string): Promise<T> {
    return this.settle(this.http.get<T>(url));
  }

  private post<T extends Pending>(url: string, body: object): Promise<T> {
    return this.settle(this.http.post<T>(url, body));
  }

  private async settle<T extends Pending>(call: Observable<T>): Promise<T> {
    try {
      const reply = await firstValueFrom(call);
      this.inTransaction.set(reply.inTransaction);
      return reply;
    } catch (failure) {
      if (!(failure instanceof HttpErrorResponse)) {
        throw failure;
      }
      const body = failure.error as Partial<Pending> | null;
      if (body && typeof body.inTransaction === 'boolean') {
        this.inTransaction.set(body.inTransaction);
      }
      throw new ApiError(describeFailure(failure), failure.status);
    }
  }
}
