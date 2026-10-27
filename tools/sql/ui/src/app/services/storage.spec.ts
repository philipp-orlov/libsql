import { Cell } from '../models/api.models';
import { Catalog, Reply, SocketResult } from '../remote';
import { Conn } from './catalog';
import * as storage from './storage';

/** A result with these rows; blob cells may be given as bytes (what `raw` returns). */
function result(rows: (Cell | Uint8Array)[][], done: Partial<Reply> = {}): SocketResult {
  const width = rows[0]?.length ?? 0;
  return {
    view: {
      columns: Array.from({ length: width }, () => ''),
      rowCount: rows.length,
      cell: (r: number, c: number) => {
        const v = rows[r][c];
        return v instanceof Uint8Array ? { type: 'blob', size: v.length, value: '' } : v;
      },
      raw: (r: number, c: number) => rows[r][c] as Uint8Array,
    } as never,
    done: done as Reply,
  };
}

class FakeConn implements Conn {
  sql: string[] = [];
  constructor(private readonly answer: (sql: string) => SocketResult = () => result([])) {}
  async exec(sql: string): Promise<SocketResult> {
    this.sql.push(sql);
    return this.answer(sql);
  }
  async describe(_what: Catalog, _name?: string): Promise<SocketResult> {
    return result([]);
  }
}

const bytes = (...values: number[]) => Uint8Array.from(values);
const text = (s: string) => new TextEncoder().encode(s);

describe('storage', () => {
  describe('the store', () => {
    it('reads the counters by name', async () => {
      const conn = new FakeConn(() => result([['page_size', 4096], ['free_pages', 7]]));
      expect(await storage.loadStats(conn)).toEqual({ page_size: 4096, free_pages: 7 });
    });

    it('reads the trees with the SQL object each holds', async () => {
      const conn = new FakeConn(() =>
        result([
          [0, '', 'free', 'free', null, null, 0, 0, 0, 0, 0, 0, 0],
          [5, 't#1', 'named', 'table', 'orders', null, 6, 2, 1, 54, 0, 3000, 0],
          [3, 'i#2', 'named', 'index', 'by_customer', 'orders', null, 0, 0, 0, 0, 0, 0],
        ]),
      );
      const trees = await storage.loadTrees(conn);
      expect(trees.length).toBe(3);
      expect(trees[1]).toEqual({ id: 5, name: 't#1', role: 'named', kind: 'table', object: 'orders', parent: null, root: 6, depth: 2, branchPages: 1, leafPages: 54, overflowPages: 0, entries: 3000, flags: 0 });
      expect(trees[2].parent).toBe('orders');
      expect(trees[2].root).toBeNull();
      expect(storage.treeLabel(trees[0])).toBe('Free list');
      expect(storage.treeLabel(trees[1])).toBe('orders');
      expect(storage.treePages(trees[1])).toBe(55);
    });

    it('reads the map in runs, and takes an unknown kind for unknown', async () => {
      const conn = new FakeConn(() => result([[0, 2, 'meta', null], [2, 5, 'leaf', 3], [7, 1, 'weird', null]]));
      expect(await storage.loadMap(conn)).toEqual([
        { first: 0, count: 2, kind: 'meta', tree: null },
        { first: 2, count: 5, kind: 'leaf', tree: 3 },
        { first: 7, count: 1, kind: 'unknown', tree: null },
      ]);
    });

    it('reads the free list with its first pages', async () => {
      const conn = new FakeConn(() => result([[9, 0, 4, '12,13,14,15', 0], [0, 0, 2, '30,31', 1]]));
      expect(await storage.loadFree(conn)).toEqual([
        { txn: 9, chunk: 0, pages: 4, firstPages: [12, 13, 14, 15], deferred: false },
        { txn: 0, chunk: 0, pages: 2, firstPages: [30, 31], deferred: true },
      ]);
    });

    it('reads the meta pages', async () => {
      const row = [1, 1, 1, 7, 4096, 12, 400, 2162688, 4, 'aa', 'aa', 'store', 'commit', 'parent', 'bb', 0, 2, 11];
      const [meta] = await storage.loadMeta(new FakeConn(() => result([row])));
      expect(meta.current).toBeTrue();
      expect(meta.txnid).toBe(12);
      expect(meta.checksum).toBe('aa');
      expect(meta.inlineTxn).toBe(11);
    });

    it('turns a check into a report', async () => {
      const bad = await storage.runCheck(new FakeConn(() => result([['page', 261, 'x', 155], ['page', 9, 'x', 155], ['integrity', null, 'page checksum mismatch', 155]])));
      expect(bad).toEqual({ ok: false, pagesChecked: 155, badPages: [261, 9], integrity: 'page checksum mismatch' });
      const good = await storage.runCheck(new FakeConn(() => result([['ok', null, '', 155]])));
      expect(good.ok).toBeTrue();
      expect(good.pagesChecked).toBe(155);
    });
  });

  describe('one page', () => {
    const pageRow = [6, 'branch', 1, 0, 0, 4096, 54, 124, 2800, 0, 0, 2676, 1412, 3000, 1, 0, '775c4ead189f80f6', '775c4ead189f80f6', 1, 1];

    it('reads the header', async () => {
      const conn = new FakeConn(() => result([pageRow]));
      const page = await storage.loadPage(conn, 6);
      expect(conn.sql[0]).toContain('pragma_storage_page(6)');
      expect(page.kind).toBe('branch');
      expect(page.nkeys).toBe(54);
      expect(page.freeBytes).toBe(2676);
      expect(page.hashOk && page.headerOk).toBeTrue();
      expect(page.storedHash).toBe(page.computedHash);
    });

    it('never puts anything but a number in the SQL', async () => {
      const conn = new FakeConn();
      await expectAsync(storage.loadPage(conn, '6); DROP TABLE t; --' as unknown as number)).toBeRejectedWithError(/bad page number/);
      await expectAsync(storage.loadPage(conn, -1)).toBeRejectedWithError(/bad page number/);
      await expectAsync(storage.loadPage(conn, 1.5)).toBeRejectedWithError(/bad page number/);
      expect(conn.sql).toEqual([]);
    });

    it('says when a page cannot be read', async () => {
      await expectAsync(storage.loadPage(new FakeConn(), 6)).toBeRejectedWithError(/cannot be read/);
    });

    it('reads nodes, decoding what the tree kind allows', async () => {
      const row = (key: Uint8Array) => [0, 4072, 68, key, 8, 51, text('v'), 0, 0, 0, 0, 0, 59, null, '(NULL, \'x\')'];
      const conn = new FakeConn(() => result([row(bytes(0x80, 0, 0, 0, 0, 0, 0, 59))]));
      const [node] = await storage.loadNodes(conn, 7, 'table');
      expect(conn.sql[0]).toContain('storage_rowid(key)');
      expect(conn.sql[0]).toContain('storage_row(value)');
      expect(conn.sql[0]).toContain('storage_key(key)');
      expect(node.rowid).toBe(59);
      expect(node.rowText).toBe("(NULL, 'x')");
      expect(Array.from(node.key)).toEqual([0x80, 0, 0, 0, 0, 0, 0, 59]);
      expect(node.valueBytes).toBe(51);

      const indexConn = new FakeConn(() => result([]));
      await storage.loadNodes(indexConn, 7, 'index');
      expect(indexConn.sql[0]).toContain('storage_key(key, 1)');
      expect(indexConn.sql[0]).not.toContain('storage_rowid(key)');
      const plainConn = new FakeConn(() => result([]));
      await storage.loadNodes(plainConn, 7, null);
      expect(plainConn.sql[0]).not.toContain('storage_row');
    });

    it('reads the bytes of a page', async () => {
      const conn = new FakeConn(() => result([[bytes(1, 2, 3), 1]]));
      const { bytes: data, pages } = await storage.loadBytes(conn, 2);
      expect(Array.from(data)).toEqual([1, 2, 3]);
      expect(pages).toBe(1);
    });

    it('reads the children of a branch with their headers', async () => {
      const conn = new FakeConn(() => result([[278, 'leaf', 42, 1000, 3096, 4096, 42, 1, 1]]));
      const [child] = await storage.loadChildren(conn, 6);
      expect(conn.sql[0]).toContain('pragma_storage_page(n.child)');
      expect(child).toEqual({ pgno: 278, kind: 'leaf', nkeys: 42, freeBytes: 1000, usedBytes: 3096, pageSize: 4096, entries: 42, hashOk: true, headerOk: true });
    });
  });

  describe('presentation', () => {
    it('names the flags', () => {
      expect(storage.flagNames(1)).toEqual(['BRANCH']);
      expect(storage.flagNames(2 | 16)).toEqual(['LEAF', 'PREFIX']);
      expect(storage.flagNames(0)).toEqual([]);
    });

    it('shows text as text and everything else as hex', () => {
      expect(storage.printable(text('héllo'))).toBe('héllo');
      expect(storage.printable(bytes(0xff, 0xfe))).toBeNull();
      expect(storage.printable(bytes(0, 1, 2))).toBeNull();
      expect(storage.printable(bytes())).toBe('');
      expect(storage.toHex(bytes(0, 15, 255))).toBe('00 0f ff');
      expect(storage.toHex(bytes(1, 2, 3, 4), 2)).toBe('01 02 …');
      expect(storage.preview(text('abc'), 3)).toBe('abc');
      expect(storage.preview(text('abc'), 10)).toBe('abc …');
      expect(storage.preview(bytes(0x80, 0, 1), 3)).toBe('80 00 01');
    });

    it('formats sizes', () => {
      expect(storage.formatBytes(0)).toBe('0 B');
      expect(storage.formatBytes(1023)).toBe('1023 B');
      expect(storage.formatBytes(4096)).toBe('4.0 KiB');
      expect(storage.formatBytes(2162688)).toBe('2.1 MiB');
      expect(storage.formatBytes(5 * 1024 ** 4)).toBe('5.0 TiB');
    });
  });
});
