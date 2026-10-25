import { provideHttpClient } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { provideZonelessChangeDetection } from '@angular/core';
import { TestBed } from '@angular/core/testing';

import { GOLDEN } from '../bufs/golden';
import { ApiError, DatabaseService } from './database.service';

describe('DatabaseService', () => {
  // The specs below read plain JSON replies; the bufs ones are further down.
  beforeEach(() => localStorage.setItem('sql.format', 'json'));
  afterEach(() => localStorage.removeItem('sql.format'));

  let service: DatabaseService;
  let http: HttpTestingController;

  beforeEach(() => {
    TestBed.configureTestingModule({
      providers: [provideZonelessChangeDetection(), provideHttpClient(), provideHttpClientTesting()],
    });
    service = TestBed.inject(DatabaseService);
    http = TestBed.inject(HttpTestingController);
  });

  afterEach(() => http.verify());

  // A request that follows an earlier one is sent a few ticks after the earlier one is answered.
  async function next(url: string) {
    for (let attempt = 0; attempt < 50; attempt++) {
      const [found] = http.match(url);
      if (found) {
        return found;
      }
      await new Promise(resolve => setTimeout(resolve));
    }
    throw new Error(`no request to ${url}`);
  }

  const emptySchema = { inTransaction: false, tables: [], views: [], indexes: [], triggers: [] };

  it('loads the info and the schema', async () => {
    const loading = service.load();
    http.expectOne('/api/info').flush({ path: '/x.db', version: '1', readOnly: true, inTransaction: false });
    (await next('/api/schema')).flush({ ...emptySchema, tables: [{ name: 't', sql: '', strict: false, withoutRowid: false, columns: [] }] });
    await loading;
    expect(service.info()?.path).toBe('/x.db');
    expect(service.info()?.readOnly).toBeTrue();
    expect(service.schema().tables.map(table => table.name)).toEqual(['t']);
  });

  it('follows the pending state of every reply', async () => {
    const run = service.execute('UPDATE t SET a = 1');
    http.expectOne('/api/sql').flush({ inTransaction: true, columns: [], rows: [], rowCount: 0, truncated: false, changes: 1, lastInsertId: 0, elapsedMs: 1 });
    (await next('/api/schema')).flush({ ...emptySchema, inTransaction: true });
    await run;
    expect(service.inTransaction()).toBeTrue();

    const commit = service.commit();
    http.expectOne('/api/commit').flush({ inTransaction: false });
    (await next('/api/schema')).flush(emptySchema);
    await commit;
    expect(service.inTransaction()).toBeFalse();
  });

  it('posts what execute is given', async () => {
    const run = service.execute('SELECT 1');
    const request = http.expectOne('/api/sql');
    expect(request.request.method).toBe('POST');
    expect(request.request.body).toEqual({ sql: 'SELECT 1' });
    request.flush({ inTransaction: false, columns: ['1'], rows: [[1]], rowCount: 1, truncated: false, changes: 0, lastInsertId: 0, elapsedMs: 0 });
    const result = await run;
    expect(result.rowCount).toBe(1);
    expect(result.cell(0, 0)).toBe(1);
  });

  it('does not reload the schema after a read that left nothing pending', async () => {
    const run = service.execute('SELECT 1');
    http.expectOne('/api/sql').flush({ inTransaction: false, columns: ['1'], rows: [[1]], rowCount: 1, truncated: false, changes: 0, lastInsertId: 0, elapsedMs: 0 });
    await run;
    http.expectNone('/api/schema');
    expect(service.revision()).toBe(0);
  });

  it('reloads the views after a change', async () => {
    const run = service.execute('INSERT INTO t VALUES (1)');
    http.expectOne('/api/sql').flush({ inTransaction: true, columns: [], rows: [], rowCount: 0, truncated: false, changes: 1, lastInsertId: 1, elapsedMs: 0 });
    (await next('/api/schema')).flush({ ...emptySchema, inTransaction: true });
    await run;
    expect(service.revision()).toBe(1);
  });

  it('sends edits to the row endpoints and counts them as changes', async () => {
    const update = service.updateRow('t', [1], { a: 'x' });
    const updateRequest = http.expectOne('/api/rows/update');
    expect(updateRequest.request.body).toEqual({ table: 't', key: [1], set: { a: 'x' } });
    updateRequest.flush({ inTransaction: true, changes: 1 });
    await update;

    const insert = service.insertRow('t', { a: null });
    const insertRequest = http.expectOne('/api/rows/insert');
    expect(insertRequest.request.body).toEqual({ table: 't', values: { a: null } });
    insertRequest.flush({ inTransaction: true, changes: 1 });
    await insert;

    const remove = service.deleteRows('t', [[1], [2]]);
    const removeRequest = http.expectOne('/api/rows/delete');
    expect(removeRequest.request.body).toEqual({ table: 't', keys: [[1], [2]] });
    removeRequest.flush({ inTransaction: true, changes: 2 });
    await remove;
    expect(service.revision()).toBe(3);
    expect(service.inTransaction()).toBeTrue();
  });

  it('turns the error of the server into an ApiError and still learns the pending state', async () => {
    service.inTransaction.set(false);
    const run = service.execute('SELECT * FROM nope');
    http.expectOne('/api/sql').flush({ error: 'no such table: nope', inTransaction: true }, { status: 400, statusText: 'Bad Request' });
    await expectAsync(run).toBeRejectedWith(jasmine.objectContaining({ message: 'no such table: nope', status: 400 }));
    expect(service.inTransaction()).toBeTrue();
  });

  it('says when the server cannot be reached', async () => {
    const run = service.browse({ table: 't', offset: 0, limit: 10, sort: [], filters: {} });
    http.expectOne('/api/browse').error(new ProgressEvent('error'), { status: 0 });
    const failure: unknown = await run.then(() => null, (error: unknown) => error);
    expect(failure instanceof ApiError).toBeTrue();
    expect((failure as ApiError).message).toBe('The server cannot be reached.');
  });

  it('reads and sets pragmas', async () => {
    const list = service.pragmas();
    http.expectOne('/api/pragmas').flush({ inTransaction: false, pragmas: [] });
    expect((await list).pragmas).toEqual([]);

    const set = service.setPragma('foreign_keys', 1);
    const request = http.expectOne('/api/pragmas');
    expect(request.request.method).toBe('POST');
    expect(request.request.body).toEqual({ name: 'foreign_keys', value: 1 });
    request.flush({ inTransaction: false });
    await set;
  });

  it('fetches the dump script', async () => {
    const dump = service.dump();
    http.expectOne('/api/dump').flush({ inTransaction: false, sql: 'BEGIN TRANSACTION;\nCOMMIT;\n' });
    expect(await dump).toBe('BEGIN TRANSACTION;\nCOMMIT;\n');
  });
});


describe('DatabaseService.execute over libbufs', () => {
  let service: DatabaseService;
  let http: HttpTestingController;

  const base64 = (text: string): ArrayBuffer => Uint8Array.from(atob(text), c => c.charCodeAt(0)).buffer;
  const text = (value: unknown): ArrayBuffer => new TextEncoder().encode(JSON.stringify(value)).buffer as ArrayBuffer;
  const BUFS = { 'Content-Type': 'application/vnd.libsql.bufs', 'X-Sql-In-Transaction': 'false', 'X-Sql-Elapsed-Ms': '1.500' };

  beforeEach(() => {
    localStorage.removeItem('sql.format');
    TestBed.configureTestingModule({
      providers: [provideZonelessChangeDetection(), provideHttpClient(), provideHttpClientTesting()],
    });
    service = TestBed.inject(DatabaseService);
    http = TestBed.inject(HttpTestingController);
  });

  afterEach(() => http.verify());

  it('asks for libbufs by default and reads the cells out of the bytes', async () => {
    const golden = GOLDEN.find(g => g.name === 'text_dict')!;
    const run = service.execute('SELECT status FROM t');
    const request = http.expectOne('/api/sql');
    expect(request.request.body).toEqual({ sql: 'SELECT status FROM t', format: 'bufs' });
    expect(request.request.responseType).toBe('arraybuffer');
    request.flush(base64(golden.bufs), { headers: BUFS });
    const result = await run;
    expect(result.columns).toEqual(['status']);
    expect(result.rowCount).toBe(golden.rows.length);
    expect(result.totalRows).toBe(golden.rows.length);
    expect(result.elapsedMs).toBeCloseTo(1.5);
    expect(result.inTransaction).toBeFalse();
    for (let r = 0; r < golden.rows.length; r++) {
      expect(result.cell(r, 0)).toEqual(golden.rows[r][0] as never);
    }
    http.expectNone('/api/schema');
  });

  it('follows the pending state from the headers and reloads the schema', async () => {
    const golden = GOLDEN.find(g => g.name === 'no_columns')!;
    const run = service.execute('UPDATE t SET a = a');
    http.expectOne('/api/sql').flush(base64(golden.bufs), { headers: { ...BUFS, 'X-Sql-In-Transaction': 'true' } });
    let schema = http.match('/api/schema');
    for (let attempt = 0; attempt < 50 && schema.length === 0; attempt++) {
      await new Promise(resolve => setTimeout(resolve));
      schema = http.match('/api/schema');
    }
    expect(schema.length).toBe(1);
    schema[0].flush({ inTransaction: true, tables: [], views: [], indexes: [], triggers: [] });
    const result = await run;
    expect(service.inTransaction()).toBeTrue();
    expect(result.changes).toBe(1);
    expect(result.columns).toEqual([]);
  });

  it('still reads JSON when the server does not know the format', async () => {
    const run = service.execute('SELECT 1');
    http.expectOne('/api/sql').flush(text({ inTransaction: false, columns: ['1'], rows: [[1]], rowCount: 1, truncated: false, changes: 0, lastInsertId: 0, elapsedMs: 2 }), {
      headers: { 'Content-Type': 'application/json' },
    });
    const result = await run;
    expect(result.cell(0, 0)).toBe(1);
    expect(result.totalRows).toBe(1);
  });

  it('turns a JSON error into an ApiError even though bytes were asked for', async () => {
    service.inTransaction.set(false);
    const run = service.execute('SELECT * FROM nope');
    http.expectOne('/api/sql').flush(text({ error: 'no such table: nope', inTransaction: true }), { status: 400, statusText: 'Bad Request' });
    await expectAsync(run).toBeRejectedWith(jasmine.objectContaining({ message: 'no such table: nope', status: 400 }));
    expect(service.inTransaction()).toBeTrue();
  });

  it('rejects bytes that are not a result', async () => {
    const run = service.execute('SELECT 1');
    http.expectOne('/api/sql').flush(new ArrayBuffer(16), { headers: BUFS });
    await expectAsync(run).toBeRejectedWith(jasmine.any(ApiError));
  });
});
