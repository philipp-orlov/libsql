import { provideZonelessChangeDetection } from '@angular/core';
import { TestBed } from '@angular/core/testing';

import { GOLDEN } from '../bufs/golden';
import { ResultSetView } from '../bufs';
import { Catalog, ErrorCode, ParamValue, Reply, ResultOptionsFields, SocketError, SocketResult } from '../remote';
import { ApiError, DatabaseService, QuerySocket, SOCKET_PATH } from './database.service';

describe('DatabaseService', () => {
  let service: DatabaseService;
  let fake: FakeQuerySocket;
  let urls: { url: string; token: string | undefined }[];

  const golden = GOLDEN.find(g => g.name === 'text_plain')!;
  const view = () => ResultSetView.open(Uint8Array.from(atob(golden.bufs), c => c.charCodeAt(0)).buffer);
  const done = (patch: Partial<Reply> = {}): Reply => ({ elapsedUs: 2500, inTransaction: false, changes: 0, lastInsertId: 0, sql: '', strict: false, withoutRowid: false, ...patch }) as Reply;

  /** Answers every statement with the golden rows; a script can say otherwise per statement. */
  class FakeQuerySocket implements QuerySocket {
    connected = false;
    welcome: Reply | null = { server: 'sql/1.2.3', readOnly: false } as Reply;
    sent: string[] = [];
    connectError: Error | null = null;
    failure: Error | null = null;
    inTransaction = false;
    answer: (sql: string) => SocketResult | null = () => null;
    connect = async () => {
      if (this.connectError) {
        throw this.connectError;
      }
      this.connected = true;
    };
    exec = async (sql: string, _params?: ParamValue[], _options?: ResultOptionsFields): Promise<SocketResult> => {
      this.sent.push(sql);
      if (this.failure) {
        throw this.failure;
      }
      return this.answer(sql) ?? { view: view(), done: done({ inTransaction: this.inTransaction }) };
    };
    describe = async (_what: Catalog, _name?: string): Promise<SocketResult> => ({ view: null, done: done({ inTransaction: this.inTransaction }) });
    interrupt = jasmine.createSpy('interrupt');
    close = jasmine.createSpy('close');
  }

  beforeEach(() => {
    sessionStorage.removeItem('sql.token');
    TestBed.configureTestingModule({ providers: [provideZonelessChangeDetection()] });
    service = TestBed.inject(DatabaseService);
    fake = new FakeQuerySocket();
    urls = [];
    service.socketFactory = (url, token) => {
      urls.push({ url, token });
      return fake;
    };
  });

  afterEach(() => sessionStorage.removeItem('sql.token'));

  it('connects to the SQL route of the page’s own server', async () => {
    await service.execute('SELECT 1');
    expect(urls.length).toBe(1);
    expect(urls[0].url).toBe(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}${SOCKET_PATH}`);
    expect(urls[0].token).toBeUndefined();
  });

  it('sends the statement and reads the rows from the frame', async () => {
    const result = await service.execute('SELECT name FROM t');
    expect(fake.sent).toEqual(['SELECT name FROM t']);
    expect(result.columns).toEqual(['name']);
    expect(result.rowCount).toBe(golden.rows.length);
    expect(result.elapsedMs).toBeCloseTo(2.5);
    expect(result.cell(0, 0)).toEqual(golden.rows[0][0] as never);
  });

  it('reports the counters of a statement without rows', async () => {
    fake.answer = () => ({ view: null, done: done({ changes: 3, lastInsertId: 9 }) });
    const result = await service.execute('DELETE FROM t');
    expect(result.rowCount).toBe(0);
    expect(result.changes).toBe(3);
    expect(result.lastInsertId).toBe(9);
  });

  it('loads the info from the welcome and the catalog, and the schema', async () => {
    fake.welcome = { server: 'sql/1.2.3', readOnly: true } as Reply;
    fake.answer = sql => {
      if (sql.includes('pragma_function_list')) {
        return { view: { columns: ['name'], rowCount: 2, cell: (r: number) => (r ? 'count' : 'abs') } as never, done: done() };
      }
      if (sql.includes('pragma_database_list')) {
        return { view: { columns: ['file'], rowCount: 1, cell: () => '/data/x.db' } as never, done: done() };
      }
      return { view: null, done: done() };
    };
    await service.load();
    expect(service.info()).toEqual({ path: '/data/x.db', version: '1.2.3', readOnly: true, functions: ['abs', 'count'], inTransaction: false });
    expect(service.schema().tables).toEqual([]);
  });

  it('follows the pending state the socket reports, and reloads the views', async () => {
    fake.inTransaction = true;
    await service.execute('UPDATE t SET a = 1');
    expect(service.inTransaction()).toBeTrue();
    expect(service.revision()).toBe(1);
    expect(fake.sent[1]).toContain('sqlite_schema'); // the schema is read again
  });

  it('leaves the views alone when nothing is left pending', async () => {
    await service.execute('SELECT 1');
    expect(service.revision()).toBe(0);
    expect(fake.sent).toEqual(['SELECT 1']);
  });

  it('writes and reverts only what is pending', async () => {
    await service.commit();
    await service.revert();
    expect(fake.sent.filter(sql => sql === 'COMMIT' || sql === 'ROLLBACK')).toEqual([]);
    service.inTransaction.set(true);
    await service.commit();
    expect(fake.sent).toContain('COMMIT');
    expect(service.inTransaction()).toBeFalse();
    service.inTransaction.set(true);
    await service.revert();
    expect(fake.sent).toContain('ROLLBACK');
  });

  it('turns a refused statement into an ApiError and keeps the pending state', async () => {
    service.inTransaction.set(true);
    fake.failure = new SocketError('no such table: x', ErrorCode.Runtime);
    const failure = await service.execute('SELECT * FROM x').catch(e => e);
    expect(failure instanceof ApiError).toBeTrue();
    expect((failure as ApiError).message).toBe('no such table: x');
    expect((failure as ApiError).status).toBe(400);
    expect(service.inTransaction()).toBeTrue();
  });

  it('knows that a lost connection rolled the changes back', async () => {
    service.inTransaction.set(true);
    fake.failure = new SocketError('the connection was lost', ErrorCode.Connection);
    const failure = await service.execute('SELECT 1').catch(e => e);
    expect((failure as ApiError).status).toBe(0);
    expect(service.inTransaction()).toBeFalse();
  });

  it('says so when the server cannot be reached, and tries again next time', async () => {
    fake.connectError = new SocketError('cannot open', ErrorCode.Connection);
    const failure = await service.execute('SELECT 1').catch(e => e);
    expect((failure as ApiError).status).toBe(0);
    fake.connectError = null;
    await service.execute('SELECT 1');
    expect(urls.length).toBe(2);
  });

  it('asks for a token when the server wants one, and connects again with it', async () => {
    fake.connectError = new SocketError('token missing', ErrorCode.PermissionDenied);
    const failure = await service.load().catch(e => e);
    expect((failure as ApiError).status).toBe(401);
    expect(service.authRequired()).toBeTrue();

    fake.connectError = null;
    await service.useToken('secret');
    expect(service.authRequired()).toBeFalse();
    expect(urls.at(-1)!.token).toBe('secret');
    expect(sessionStorage.getItem('sql.token')).toBe('secret');
    expect(service.info()?.version).toBe('1.2.3');
  });

  it('presents a stored token', async () => {
    sessionStorage.setItem('sql.token', 'kept');
    await service.execute('SELECT 1');
    expect(urls[0].token).toBe('kept');
  });

  it('forwards an interrupt to the socket', async () => {
    await service.execute('SELECT 1');
    service.interruptRunning();
    expect(fake.interrupt).toHaveBeenCalled();
  });

  it('refuses a pragma change while changes are pending', async () => {
    service.inTransaction.set(true);
    await expectAsync(service.setPragma('foreign_keys', 1)).toBeRejectedWithError(/pending changes/);
  });
});
