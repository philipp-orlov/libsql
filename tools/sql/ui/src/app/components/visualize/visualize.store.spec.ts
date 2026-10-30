import { provideZonelessChangeDetection, signal } from '@angular/core';
import { TestBed } from '@angular/core/testing';

import { DatabaseService } from '../../services/database.service';
import { MapRun, StorageTree } from '../../services/storage';
import { VisualizeStore } from './visualize.store';

const tree = (id: number, kind: StorageTree['kind'], object: string | null = null): StorageTree => ({
  id, name: '', role: 'named', kind, object, parent: null, root: 2, depth: 1, branchPages: 0, leafPages: 1, overflowPages: 0, entries: 1, flags: 0,
});

describe('VisualizeStore', () => {
  let store: VisualizeStore;

  // meta 0-1, leaf 2-3 (tree 5), branch 4 (tree 5), overflow 5 with its continuation 6-8, free 9-12
  const runs: MapRun[] = [
    { first: 0, count: 2, kind: 'meta', tree: null },
    { first: 2, count: 2, kind: 'leaf', tree: 5 },
    { first: 4, count: 1, kind: 'branch', tree: 5 },
    { first: 5, count: 1, kind: 'overflow', tree: 5 },
    { first: 6, count: 3, kind: 'overflow-continued', tree: 5 },
    { first: 9, count: 4, kind: 'free', tree: null },
  ];

  beforeEach(() => {
    TestBed.configureTestingModule({
      providers: [provideZonelessChangeDetection(), { provide: DatabaseService, useValue: { conn: { exec: async () => ({ view: null, done: {} }), describe: async () => ({ view: null, done: {} }) }, revision: signal(0), inTransaction: signal(false) } }],
    });
    store = TestBed.inject(VisualizeStore);
    store.runs.set(runs);
    store.trees.set([tree(5, 'table', 'orders')]);
    store.stats.set({ page_size: 4096, used_pages: 13 });
  });

  it('counts the pages of each kind', () => {
    const totals = store.totals();
    expect(totals.meta).toBe(2);
    expect(totals.leaf).toBe(2);
    expect(totals['overflow-continued']).toBe(3);
    expect(totals.free).toBe(4);
    expect(store.usedPages()).toBe(13);
    expect(store.pageSize()).toBe(4096);
  });

  it('finds the run a page lies in, and its tree', () => {
    expect(store.ownerOf(0)?.kind).toBe('meta');
    expect(store.ownerOf(3)).toEqual({ kind: 'leaf', tree: store.trees()[0], runFirst: 2, runCount: 2 });
    expect(store.ownerOf(12)?.kind).toBe('free');
    expect(store.ownerOf(12)?.tree).toBeNull();
    expect(store.ownerOf(13)).toBeNull();
  });

  it('opens a page and keeps the explorer on its tree', () => {
    store.select(3);
    expect(store.selectedPage()).toBe(3);
    expect(store.selectedTree()).toBe(5);
    store.select(10);
    expect(store.selectedTree()).toBe(5); // a free page belongs to no tree: the choice stays
  });

  it('opens the first page of an overflow run for any page in it', () => {
    store.select(7);
    expect(store.selectedPage()).toBe(5);
  });

  it('goes back and forward through the pages visited', () => {
    store.select(2);
    store.select(3);
    store.select(4);
    expect(store.canGoBack()).toBeTrue();
    store.back();
    expect(store.selectedPage()).toBe(3);
    store.back();
    expect(store.selectedPage()).toBe(2);
    expect(store.canGoBack()).toBeFalse();
    store.forward();
    expect(store.selectedPage()).toBe(3);
    store.select(10); // a new visit ends the forward history
    expect(store.canGoForward()).toBeFalse();
  });

  it('does not repeat the page already open in the history', () => {
    store.select(2);
    store.select(2);
    expect(store.canGoBack()).toBeFalse();
  });

  it('marks the pages a check found bad', () => {
    store.check.set({ ok: false, pagesChecked: 13, badPages: [3, 4], integrity: '' });
    expect(store.badPages().has(3)).toBeTrue();
    expect(store.badPages().has(2)).toBeFalse();
  });
});
