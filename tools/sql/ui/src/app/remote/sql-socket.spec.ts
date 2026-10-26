import { GOLDEN } from '../bufs/golden';
import { ErrorCode, Kind, Op } from './layout';
import { REPLIES } from './golden';
import { SocketError, SqlSocket, WebSocketLike } from './sql-socket';
import { encodeRequest } from './request';

function bytes(base64: string): Uint8Array {
  const text = atob(base64);
  const out = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) {
    out[i] = text.charCodeAt(i);
  }
  return out;
}

/** A golden reply with the request id it answers; optionally followed by a ResultSet message (a Rows frame). */
function reply(name: string, id: number, rows?: string, patch?: (view: DataView) => void): ArrayBuffer {
  const head = bytes(REPLIES.find(r => r.name === name)!.bytes);
  const tail = rows ? bytes(rows) : new Uint8Array(0);
  const out = new Uint8Array(head.length + tail.length);
  out.set(head, 0);
  out.set(tail, head.length);
  const view = new DataView(out.buffer);
  view.setUint32(12, id, true); // Reply.id: root table at 8, field at 4
  patch?.(view);
  return out.buffer;
}

const rowsOf = (name: string) => GOLDEN.find(g => g.name === name)!.bufs;
const opOf = (frame: ArrayBuffer) => new DataView(frame).getUint8(16);
const idOf = (frame: ArrayBuffer) => new DataView(frame).getUint32(12, true);

class FakeSocket implements WebSocketLike {
  binaryType = '';
  readyState = 0;
  onopen: ((event: unknown) => void) | null = null;
  onmessage: ((event: { data: unknown }) => void) | null = null;
  onclose: ((event: { code?: number; reason?: string }) => void) | null = null;
  onerror: ((event: unknown) => void) | null = null;
  sent: ArrayBuffer[] = [];

  constructor(private readonly script: (socket: FakeSocket, frame: ArrayBuffer) => void) {
    Promise.resolve().then(() => {
      this.readyState = 1;
      this.onopen?.({});
    });
  }
  send(data: ArrayBuffer): void {
    this.sent.push(data);
    Promise.resolve().then(() => this.script(this, data));
  }
  receive(frame: ArrayBuffer): void {
    this.onmessage?.({ data: frame });
  }
  close(code?: number, reason?: string): void {
    this.readyState = 3;
    this.onclose?.({ code, reason });
  }
  drop(): void {
    this.readyState = 3;
    this.onclose?.({ code: 1006 });
  }
}

/** A server that welcomes, then answers Exec with `exec`. */
function server(exec: (socket: FakeSocket, id: number) => void): (socket: FakeSocket, frame: ArrayBuffer) => void {
  return (socket, frame) => {
    if (opOf(frame) === Op.Hello) {
      socket.receive(reply('welcome', idOf(frame)));
    } else if (opOf(frame) === Op.Exec) {
      exec(socket, idOf(frame));
    }
  };
}

describe('SqlSocket', () => {
  let sockets: FakeSocket[];
  const make = (script: (socket: FakeSocket, frame: ArrayBuffer) => void, options: Partial<ConstructorParameters<typeof SqlSocket>[1]> = {}) => {
    sockets = [];
    return new SqlSocket('ws://x/api/v1/ws/sql', {
      pingMs: 0,
      createSocket: () => {
        const socket = new FakeSocket(script);
        sockets.push(socket);
        return socket;
      },
      ...options,
    });
  };

  it('says hello with the protocol, a client name and the token', async () => {
    const socket = make(server(() => undefined), { token: 'abc' });
    await socket.connect();
    expect(sockets[0].binaryType).toBe('arraybuffer');
    expect(new Uint8Array(sockets[0].sent[0])).toEqual(
      new Uint8Array(encodeRequest({ id: 1, op: Op.Hello, protocol: 1, client: 'sql-ui/1.0', token: 'abc', window: 1 << 20, maxMessage: 16 << 20 })),
    );
  });

  it('runs a statement and reads the rows out of the frame', async () => {
    const golden = rowsOf('text_dict');
    const socket = make(
      server((s, id) => {
        s.receive(reply('rows', id, golden));
        s.receive(reply('done', id));
      }),
    );
    const result = await socket.exec('SELECT status FROM t');
    expect(result.view!.columns).toEqual(['status']);
    expect(result.view!.rowCount).toBe(GOLDEN.find(g => g.name === 'text_dict')!.rows.length);
    expect(result.view!.cell(0, 0)).toEqual(GOLDEN.find(g => g.name === 'text_dict')!.rows[0][0] as never);
    expect(result.done.kind).toBe(Kind.Done);
    expect(result.done.inTransaction).toBeTrue();
    expect(result.done.elapsedUs).toBe(1234567);
    // The request it sent: Exec with the managed hint.
    const sent = sockets[0].sent.find(f => opOf(f) === Op.Exec)!;
    expect(new Uint8Array(sent)).toEqual(new Uint8Array(encodeRequest({ id: idOf(sent), op: Op.Exec, sql: 'SELECT status FROM t', hints: 1 })));
  });

  it('joins the batches of a result', async () => {
    const golden = GOLDEN.find(g => g.name === 'batches')!;
    const socket = make(
      server((s, id) => {
        // The golden stream is three messages; send each in its own Rows frame.
        const stream = bytes(golden.bufs);
        const view = new DataView(stream.buffer);
        let at = 0;
        while (at < stream.length) {
          const size = view.getUint32(at, true);
          s.receive(reply('rows', id, btoa(String.fromCharCode(...stream.subarray(at, at + size)))));
          at += size;
        }
        s.receive(reply('done', id));
      }),
    );
    const result = await socket.exec('SELECT id, status, price FROM t');
    expect(result.view!.rowCount).toBe(golden.rows.length);
    expect(result.view!.batchCount).toBe(3);
    expect(result.view!.cell(golden.rows.length - 1, 0)).toEqual(golden.rows[golden.rows.length - 1][0] as never);
  });

  it('rejects with the server error', async () => {
    const socket = make(server((s, id) => s.receive(reply('error', id))));
    const failure = await socket.exec('INSERT').then(() => null, (e: unknown) => e);
    expect(failure instanceof SocketError).toBeTrue();
    expect((failure as SocketError).message).toBe('UNIQUE constraint failed: t.a €');
    expect((failure as SocketError).code).toBe(11);
    // And it is usable afterwards.
    expect(sockets.length).toBe(1);
  });

  it('runs one request at a time, in order', async () => {
    const order: string[] = [];
    const socket = make(
      server((s, id) => {
        order.push('exec ' + id);
        s.receive(reply('done', id));
      }),
    );
    const a = socket.exec('SELECT 1').then(() => order.push('a done'));
    const b = socket.exec('SELECT 2').then(() => order.push('b done'));
    await Promise.all([a, b]);
    expect(order.map(o => o.split(' ')[0] + ' ' + o.split(' ')[1].replace(/\d+/, ''))).toEqual(['exec ', 'a done', 'exec ', 'b done']);
  });

  it('gives credit back after reading half a window', async () => {
    const golden = rowsOf('int_u8');
    const socket = make(
      server((s, id) => {
        for (let i = 0; i < 70; i++) {
          s.receive(reply('rows', id, golden)); // each counts at least 8 KiB: 70 of them pass half of 1 MiB
        }
        s.receive(reply('done', id));
      }),
    );
    await socket.exec('SELECT small FROM t');
    const credits = sockets[0].sent.filter(f => opOf(f) === Op.Credit);
    expect(credits.length).toBeGreaterThan(0);
    // What was not credited yet goes ahead of the next request.
    await socket.exec('SELECT 1');
    expect(sockets[0].sent.filter(f => opOf(f) === Op.Credit).length).toBeGreaterThan(credits.length - 1);
  });

  it('interrupts the running request', async () => {
    let release: (() => void) | undefined;
    const socket = make(
      server((s, id) => {
        release = () => s.receive(reply('error', id, undefined, view => view.setUint16(8 + 10, ErrorCode.Interrupted, true)));
      }),
    );
    const running = socket.exec('SELECT slow').then(() => null, (e: unknown) => e);
    await new Promise(resolve => setTimeout(resolve));
    socket.interrupt();
    const interrupt = sockets[0].sent.find(f => opOf(f) === Op.Interrupt)!;
    expect(interrupt).toBeDefined();
    const target = new DataView(interrupt).getUint32(8 + 20, true);
    const exec = sockets[0].sent.find(f => opOf(f) === Op.Exec)!;
    expect(target).toBe(idOf(exec));
    release!();
    const failure = (await running) as SocketError;
    expect(failure.code).toBe(ErrorCode.Interrupted);
  });

  it('pings while connected', async () => {
    jasmine.clock().install();
    try {
      const socket = make(server(() => undefined), { pingMs: 1000 });
      const connecting = socket.connect();
      await Promise.resolve();
      await Promise.resolve();
      await connecting;
      jasmine.clock().tick(2500);
      expect(sockets[0].sent.filter(f => opOf(f) === Op.Ping).length).toBe(2);
      socket.close();
      jasmine.clock().tick(5000);
      expect(sockets[0].sent.filter(f => opOf(f) === Op.Ping).length).toBe(2);
    } finally {
      jasmine.clock().uninstall();
    }
  });

  it('reconnects by itself when the server closed an idle connection', async () => {
    // The golden Done says inTransaction; clear it.
    const plain = make(
      server((s, id) => {
        s.receive(reply('done', id, undefined, view => view.setUint8(8 + 12, 0))); // clear in_transaction
      }),
    );
    await plain.exec('SELECT 1');
    sockets[0].drop();
    await plain.exec('SELECT 2');
    expect(sockets.length).toBe(2);
  });

  it('does not reconnect inside a transaction', async () => {
    const socket = make(server((s, id) => s.receive(reply('done', id)))); // the golden Done says in_transaction
    await socket.exec('BEGIN');
    sockets[0].drop();
    const failure = (await socket.exec('SELECT 1').then(() => null, (e: unknown) => e)) as SocketError;
    expect(failure.message).toContain('rolled back');
    expect(sockets.length).toBe(1);
    // The next call starts over on a new connection.
    await socket.exec('SELECT 2');
    expect(sockets.length).toBe(2);
  });

  it('fails the running request when the connection drops', async () => {
    const socket = make(server(() => undefined));
    const running = socket.exec('SELECT slow').then(() => null, (e: unknown) => e);
    await new Promise(resolve => setTimeout(resolve));
    sockets[0].drop();
    expect(((await running) as SocketError).code).toBe(ErrorCode.Connection);
  });

  it('refuses a hello the server answers with an error', async () => {
    const socket = make((s, frame) => {
      if (opOf(frame) === Op.Hello) {
        s.receive(reply('error', idOf(frame), undefined, view => view.setUint16(8 + 10, ErrorCode.PermissionDenied, true)));
      }
    });
    const failure = (await socket.connect().then(() => null, (e: unknown) => e)) as SocketError;
    expect(failure.code).toBe(ErrorCode.PermissionDenied);
  });
});
