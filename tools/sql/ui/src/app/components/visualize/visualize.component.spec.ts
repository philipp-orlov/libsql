import { provideZonelessChangeDetection, signal } from '@angular/core';
import { TestBed } from '@angular/core/testing';

import { Cell } from '../../models/api.models';
import { DatabaseService } from '../../services/database.service';
import { query, settle } from '../../testing/test-helpers';
import { VisualizeComponent } from './visualize.component';

type Row = (Cell | Uint8Array)[];

/** A result with these rows, in the shape the storage functions read (the part of ResultSetView they use). */
function result(rows: Row[], done: object = {}) {
  return {
    view: {
      columns: rows[0]?.map(() => '') ?? [],
      rowCount: rows.length,
      cell: (r: number, c: number) => {
        const v = rows[r][c];
        return v instanceof Uint8Array ? { type: 'blob', size: v.length, value: '' } : v;
      },
      raw: (r: number, c: number) => rows[r][c] as Uint8Array,
    },
    done,
  };
}

const enc = (s: string) => new TextEncoder().encode(s);

/** A small store: meta 0-1, the root branch 2 of table "orders" over leaves 3-4, free pages 5-6. */
function fakeStore() {
  const calls: string[] = [];
  const exec = async (sql: string) => {
    calls.push(sql);
    if (sql.includes('pragma_storage_stats')) {
      return result([['page_size', 4096], ['file_size', 28672], ['used_pages', 7], ['free_pages', 2], ['last_txn', 9], ['durable_txn', 9], ['readers', 1], ['oldest_reader', 9], ['buffer_bytes', 0], ['dirty_bytes', 0]]);
    }
    if (sql.includes('pragma_storage_meta')) {
      return result([
        [0, 1, 0, 7, 4096, 8, 6, 24576, 3, 'aaaa', 'aaaa', 's', 'c0', 'c', 'bb', 0, 0, 0],
        [1, 1, 1, 7, 4096, 9, 6, 28672, 3, 'cccc', 'cccc', 's', 'c1', 'c0', 'aaaa', 0, 0, 0],
      ]);
    }
    if (sql.includes('pragma_storage_trees')) {
      return result([
        [0, '', 'free', 'free', null, null, null, 0, 0, 0, 0, 0, 0],
        [1, 't#1', 'named', 'table', 'orders', null, 2, 2, 1, 2, 0, 80, 0],
      ]);
    }
    if (sql.includes('pragma_storage_map')) {
      return result([[0, 2, 'meta', null], [2, 1, 'branch', 1], [3, 2, 'leaf', 1], [5, 2, 'free', null]]);
    }
    if (sql.includes('pragma_storage_free')) {
      return result([[8, 0, 2, '5,6', 0]]);
    }
    if (sql.includes('FROM pragma_storage_nodes(2) n, pragma_storage_page(n.child)')) {
      return result([[3, 'leaf', 40, 1000, 3096, 4096, 40, 1, 1], [4, 'leaf', 40, 900, 3196, 4096, 40, 1, 1]]);
    }
    if (sql.includes('FROM pragma_storage_nodes(2)')) {
      return result([
        [0, 4072, 16, enc(''), 0, 0, enc(''), 0, 0, 3, 40, 0, null, null, null],
        [1, 4048, 24, new Uint8Array([0x80, 0, 0, 0, 0, 0, 0, 41]), 8, 0, enc(''), 0, 0, 4, 40, 0, 41, null, null],
      ]);
    }
    if (sql.includes('FROM pragma_storage_nodes(3)')) {
      return result([[0, 4000, 68, new Uint8Array([0x80, 0, 0, 0, 0, 0, 0, 1]), 8, 51, enc('abc'), 0, 0, 0, 0, 0, 1, null, "(NULL, 'customer 1')"]]);
    }
    if (sql.includes('pragma_storage_page(2)')) {
      return result([[2, 'branch', 1, 0, 0, 4096, 2, 20, 4000, 0, 0, 3980, 116, 80, 1, 0, 'f00d', 'f00d', 1, 1]]);
    }
    if (sql.includes('pragma_storage_page(3)')) {
      return result([[3, 'leaf', 2, 0, 0, 4096, 40, 96, 1000, 0, 0, 904, 3192, 40, 1, 0, 'beef', 'dead', 0, 1]]);
    }
    if (sql.includes('pragma_storage_page(5)')) {
      return result([[5, 'leaf', 2, 0, 0, 4096, 12, 40, 3000, 0, 0, 2960, 1136, 12, 1, 0, 'aa', 'aa', 1, 1]]);
    }
    if (sql.includes('pragma_storage_bytes')) {
      return result([[new Uint8Array(4096), 1]]);
    }
    if (sql.includes('pragma_storage_check')) {
      return result([['page', 3, 'bad', 7], ['integrity', null, 'page checksum mismatch', 7]]);
    }
    return result([]);
  };
  return { calls, conn: { exec, describe: async () => result([]) } };
}

describe('VisualizeComponent', () => {
  let fake: ReturnType<typeof fakeStore>;

  beforeEach(() => {
    fake = fakeStore();
    TestBed.configureTestingModule({
      providers: [provideZonelessChangeDetection(), { provide: DatabaseService, useValue: { conn: fake.conn, revision: signal(0), inTransaction: signal(false) } }],
    });
  });

  async function create() {
    const fixture = TestBed.createComponent(VisualizeComponent);
    await fixture.whenStable();
    await settle();
    return fixture.nativeElement as HTMLElement;
  }

  const click = async (el: Element) => {
    (el as HTMLElement).click();
    await settle();
    await new Promise(resolve => setTimeout(resolve, 20));
    await settle();
  };
  const toggle = (el: HTMLElement, text: string) => [...el.querySelectorAll('mat-button-toggle')].find(t => t.textContent?.includes(text))!.querySelector('button')!;

  it('reads the snapshot once when it opens', async () => {
    await create();
    for (const name of ['stats', 'meta', 'trees', 'map', 'free']) {
      expect(fake.calls.filter(sql => sql.includes('pragma_storage_' + name)).length).toBe(1);
    }
  });

  it('shows the counters and how the pages are divided', async () => {
    const el = await create();
    const values = [...el.querySelectorAll('.stat-value')].map(e => e.textContent?.trim());
    expect(values).toContain('4.0 KiB');
    expect(values).toContain('28.0 KiB');
    expect(values).toContain('7');
    expect(values).toContain('txn 9');
    expect(el.querySelectorAll('.usage-part').length).toBe(4);
  });

  it('lists the tables, indexes and the store’s own trees', async () => {
    const el = await create();
    const titles = [...el.querySelectorAll('.group-title')].map(e => e.textContent?.trim());
    expect(titles).toEqual(['Tables', 'The store itself']);
    const names = [...el.querySelectorAll('.tree-name')].map(e => e.textContent?.trim());
    expect(names).toEqual(['orders', 'Free list']);
    expect(query<HTMLElement>(el, '.tree-pages').textContent?.trim()).toBe('3');
  });

  it('opens the pages of a tree and the root page when a tree is chosen', async () => {
    const el = await create();
    await click(query(el, '.tree-item[data-tree="1"]'));
    expect(el.querySelectorAll('app-tree-explorer [role=treeitem]').length).toBe(3); // root and its two leaves
    expect(el.querySelector('app-page-inspector .title h3')?.textContent).toContain('Page 2');
    expect(el.textContent).toContain('rowid ≥ 41');
  });

  it('takes a leaf apart: items, decoded rows and a failed hash', async () => {
    const el = await create();
    await click(query(el, '.tree-item[data-tree="1"]'));
    await click(el.querySelectorAll('app-tree-explorer [role=treeitem]')[1]);
    expect(el.querySelector('app-page-inspector .title h3')?.textContent).toContain('Page 3');
    expect(el.querySelector('app-page-inspector .hash')?.classList.contains('bad')).toBeTrue();
    expect(el.querySelector('app-page-inspector .hash')?.textContent).toContain('does not match');
    expect(el.querySelector('app-page-inspector')?.textContent).toContain("(NULL, 'customer 1')");
    expect(el.querySelector('app-page-inspector')?.textContent).toContain('rowid 1');
  });

  it('shows the meta pages, the current one marked', async () => {
    const el = await create();
    await click(toggle(el, 'Meta'));
    const cards = el.querySelectorAll('.meta-card');
    expect(cards.length).toBe(2);
    expect(cards[1].classList.contains('current')).toBeTrue();
    expect(cards[0].classList.contains('current')).toBeFalse();
  });

  it('lists the free list with links to its pages', async () => {
    const el = await create();
    await click(toggle(el, 'Free list'));
    expect(el.querySelectorAll('.free-table tbody tr').length).toBe(1);
    await click(el.querySelectorAll('.free-table .page-link')[0]);
    expect(el.querySelector('app-page-inspector .title h3')?.textContent).toContain('Page 5');
  });

  it('checks the integrity and marks the pages that failed', async () => {
    const el = await create();
    await click(query(el, 'button.check'));
    const banner = query<HTMLElement>(el, '.banner.error');
    expect(banner.textContent).toContain('1 page failed');
    expect(banner.textContent).toContain('page checksum mismatch');
    await click(banner.querySelector('.page-link')!);
    expect(el.querySelector('app-page-inspector .title h3')?.textContent).toContain('Page 3');
  });

  it('reads the snapshot again on Refresh', async () => {
    const el = await create();
    await click(query(el, 'button.refresh'));
    expect(fake.calls.filter(sql => sql.includes('pragma_storage_map')).length).toBe(2);
  });
});
