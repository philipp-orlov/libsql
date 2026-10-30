import { Injectable, computed, inject, signal } from '@angular/core';

import { DatabaseService } from '../../services/database.service';
import {
  CheckReport,
  FreeEntry,
  MapRun,
  PageDetails,
  PageKind,
  StorageMeta,
  StorageTree,
  StoreStats,
  loadFree,
  loadMap,
  loadMeta,
  loadPage,
  loadStats,
  loadTrees,
  runCheck,
} from '../../services/storage';

/** Where a page belongs, from the map. */
export interface Owner {
  kind: PageKind;
  tree: StorageTree | null;
  /** The first page of the run the page lies in, and its length. */
  runFirst: number;
  runCount: number;
}

/** The colours of the page kinds, shared by the map, the tree and the inspector. */
export const KIND_COLORS: Record<PageKind, string> = {
  meta: '#7e57c2',
  branch: '#1e88e5',
  leaf: '#43a047',
  overflow: '#fb8c00',
  'overflow-continued': '#ffb74d',
  free: '#b0bec5',
  unknown: '#e53935',
};

export const KIND_LABELS: Record<PageKind, string> = {
  meta: 'Meta',
  branch: 'Branch',
  leaf: 'Leaf',
  overflow: 'Overflow',
  'overflow-continued': 'Overflow (continued)',
  free: 'Free',
  unknown: 'Unknown',
};

export const KIND_ORDER: PageKind[] = ['meta', 'branch', 'leaf', 'overflow', 'overflow-continued', 'free', 'unknown'];

/** What the Visualize tab shows and what is selected in it; one snapshot of the store at a time. */
@Injectable({ providedIn: 'root' })
export class VisualizeStore {
  private readonly db = inject(DatabaseService);

  readonly stats = signal<StoreStats>({});
  readonly meta = signal<StorageMeta[]>([]);
  readonly trees = signal<StorageTree[]>([]);
  readonly runs = signal<MapRun[]>([]);
  readonly free = signal<FreeEntry[]>([]);
  readonly loading = signal(false);
  readonly error = signal<string | null>(null);
  /** When the snapshot was read. */
  readonly takenAt = signal<Date | null>(null);

  readonly selectedTree = signal<number | null>(null);
  readonly selectedPage = signal<number | null>(null);
  readonly check = signal<CheckReport | null>(null);
  readonly checking = signal(false);
  /** Pages visited, for Back and Forward. */
  private readonly past = signal<number[]>([]);
  private readonly future = signal<number[]>([]);
  readonly canGoBack = computed(() => this.past().length > 0);
  readonly canGoForward = computed(() => this.future().length > 0);

  readonly pageSize = computed(() => this.stats()['page_size'] ?? 0);
  readonly usedPages = computed(() => this.stats()['used_pages'] ?? 0);
  readonly badPages = computed(() => new Set(this.check()?.badPages ?? []));
  readonly currentMeta = computed(() => this.meta().find(m => m.current) ?? null);
  readonly selectedTreeInfo = computed(() => this.trees().find(t => t.id === this.selectedTree()) ?? null);

  /** Pages per kind, from the map. */
  readonly totals = computed(() => {
    const out: Record<PageKind, number> = { meta: 0, branch: 0, leaf: 0, overflow: 0, 'overflow-continued': 0, free: 0, unknown: 0 };
    for (const run of this.runs()) {
      out[run.kind] += run.count;
    }
    return out;
  });

  async refresh(): Promise<void> {
    this.loading.set(true);
    this.error.set(null);
    try {
      const conn = this.db.conn;
      const [stats, meta, trees, runs, free] = await Promise.all([loadStats(conn), loadMeta(conn), loadTrees(conn), loadMap(conn), loadFree(conn)]);
      this.stats.set(stats);
      this.meta.set(meta);
      this.trees.set(trees);
      this.runs.set(runs);
      this.free.set(free);
      this.takenAt.set(new Date());
      this.check.set(null);
      if (this.selectedTree() !== null && !trees.some(t => t.id === this.selectedTree())) {
        this.selectedTree.set(null);
      }
      const page = this.selectedPage();
      if (page !== null && page >= (stats['used_pages'] ?? 0)) {
        this.selectedPage.set(null);
      }
    } catch (failure) {
      this.error.set(failure instanceof Error ? failure.message : String(failure));
    } finally {
      this.loading.set(false);
    }
  }

  async runCheck(): Promise<CheckReport | null> {
    this.checking.set(true);
    try {
      const report = await runCheck(this.db.conn);
      this.check.set(report);
      return report;
    } catch (failure) {
      this.error.set(failure instanceof Error ? failure.message : String(failure));
      return null;
    } finally {
      this.checking.set(false);
    }
  }

  page(pgno: number): Promise<PageDetails> {
    return loadPage(this.db.conn, pgno);
  }

  /** The run a page lies in (binary search over the map). */
  ownerOf(pgno: number): Owner | null {
    const runs = this.runs();
    let lo = 0;
    let hi = runs.length - 1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      const run = runs[mid];
      if (pgno < run.first) {
        hi = mid - 1;
      } else if (pgno >= run.first + run.count) {
        lo = mid + 1;
      } else {
        return { kind: run.kind, tree: run.tree === null ? null : (this.trees().find(t => t.id === run.tree) ?? null), runFirst: run.first, runCount: run.count };
      }
    }
    return null;
  }

  /** Opens a page; a page in the middle of an overflow run opens the run's first page. */
  select(pgno: number | null, options: { treeId?: number | null; remember?: boolean } = {}): void {
    if (pgno !== null) {
      const owner = this.ownerOf(pgno);
      if (owner?.kind === 'overflow-continued') {
        pgno = this.headOfRun(pgno);
      }
      if (options.treeId === undefined && owner?.tree) {
        // keep the explorer on the tree the page is in
        this.selectedTree.set(owner.tree.id);
      }
    }
    const current = this.selectedPage();
    if (options.remember !== false && current !== null && pgno !== null && current !== pgno) {
      this.past.update(p => [...p.slice(-49), current]);
      this.future.set([]);
    }
    if (options.treeId !== undefined) {
      this.selectedTree.set(options.treeId);
    }
    this.selectedPage.set(pgno);
  }

  back(): void {
    const past = this.past();
    const current = this.selectedPage();
    if (past.length === 0) {
      return;
    }
    this.future.update(f => (current === null ? f : [current, ...f]));
    this.past.set(past.slice(0, -1));
    this.selectedPage.set(past[past.length - 1]);
  }

  forward(): void {
    const future = this.future();
    const current = this.selectedPage();
    if (future.length === 0) {
      return;
    }
    this.past.update(p => (current === null ? p : [...p, current]));
    this.future.set(future.slice(1));
    this.selectedPage.set(future[0]);
  }

  /** The first page of the overflow run `pgno` lies in. */
  private headOfRun(pgno: number): number {
    const owner = this.ownerOf(pgno);
    if (!owner) {
      return pgno;
    }
    // Runs of `overflow-continued` follow a one-page `overflow` run; the head is the page before this run.
    return owner.runFirst > 0 ? owner.runFirst - 1 : pgno;
  }
}
