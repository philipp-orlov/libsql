import { DatePipe } from '@angular/common';
import { ChangeDetectionStrategy, Component, DestroyRef, OnInit, computed, effect, inject, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatButtonToggleModule } from '@angular/material/button-toggle';
import { MatIconModule } from '@angular/material/icon';
import { MatProgressBarModule } from '@angular/material/progress-bar';
import { MatSlideToggleModule } from '@angular/material/slide-toggle';
import { MatTooltipModule } from '@angular/material/tooltip';

import { DatabaseService } from '../../services/database.service';
import { StorageTree, formatBytes, treeLabel, treePages } from '../../services/storage';
import { PageInspectorComponent } from './page-inspector.component';
import { PageMapComponent } from './page-map.component';
import { TreeExplorerComponent } from './tree-explorer.component';
import { KIND_COLORS, KIND_LABELS, KIND_ORDER, VisualizeStore } from './visualize.store';

type CentreView = 'map' | 'tree' | 'meta' | 'free';

interface Stat {
  label: string;
  value: string;
  hint: string;
  icon: string;
}

interface TreeGroup {
  title: string;
  trees: StorageTree[];
}

/**
 * The store under the database, for debugging: how the file is divided into pages, which tree each page belongs to,
 * each tree opened level by level, every page taken apart down to its bytes and its hash, the meta pages and the free
 * list, and a check of every page's hash. All of it is read over the SQL socket with PRAGMA storage_*.
 */
@Component({
  selector: 'app-visualize',
  imports: [
    DatePipe,
    MatButtonModule,
    MatButtonToggleModule,
    MatIconModule,
    MatProgressBarModule,
    MatSlideToggleModule,
    MatTooltipModule,
    PageInspectorComponent,
    PageMapComponent,
    TreeExplorerComponent,
  ],
  changeDetection: ChangeDetectionStrategy.OnPush,
  templateUrl: './visualize.component.html',
  styleUrl: './visualize.component.scss',
})
export class VisualizeComponent implements OnInit {
  protected readonly store = inject(VisualizeStore);
  protected readonly db = inject(DatabaseService);
  private readonly destroyRef = inject(DestroyRef);

  protected readonly view = signal<CentreView>('map');
  protected readonly autoRefresh = signal(false);
  protected readonly kinds = KIND_ORDER;
  protected readonly colors = KIND_COLORS;
  protected readonly labels = KIND_LABELS;

  private seenRevision = this.db.revision();

  protected readonly stats = computed<Stat[]>(() => {
    const s = this.store.stats();
    const used = s['used_pages'] ?? 0;
    const free = s['free_pages'] ?? 0;
    const size = s['page_size'] ?? 0;
    return [
      { label: 'Page size', value: formatBytes(size), hint: `${size} bytes per page`, icon: 'grid_view' },
      { label: 'File', value: formatBytes(s['file_size'] ?? 0), hint: `${s['file_size'] ?? 0} bytes allocated; ${formatBytes(s['map_size'] ?? 0)} mapped`, icon: 'storage' },
      { label: 'Pages in use', value: String(used), hint: `${formatBytes(used * size)} up to the last page in use`, icon: 'description' },
      { label: 'Free pages', value: String(free), hint: `${used ? ((free / used) * 100).toFixed(1) : 0}% of the pages in use wait on the free list for reuse`, icon: 'recycling' },
      { label: 'Last commit', value: `txn ${s['last_txn'] ?? 0}`, hint: 'The newest committed transaction', icon: 'history' },
      { label: 'Durable', value: `txn ${s['durable_txn'] ?? 0}`, hint: 'The newest commit known to be on stable storage', icon: 'verified' },
      { label: 'Readers', value: String(s['readers'] ?? 0), hint: `Live read transactions; the oldest snapshot is txn ${s['oldest_reader'] ?? 0}`, icon: 'visibility' },
      { label: 'Buffers', value: formatBytes(s['buffer_bytes'] ?? 0), hint: `Page buffers held for reuse; ${formatBytes(s['dirty_bytes'] ?? 0)} dirty`, icon: 'memory' },
    ];
  });

  protected readonly groups = computed<TreeGroup[]>(() => {
    const trees = this.store.trees();
    const byKind = (...kinds: string[]) => trees.filter(t => kinds.includes(t.kind));
    return [
      { title: 'Tables', trees: byKind('table') },
      { title: 'Indexes', trees: byKind('index') },
      { title: 'The store itself', trees: byKind('catalog', 'free', 'main', 'other') },
    ].filter(g => g.trees.length > 0);
  });

  protected readonly metaFresh = computed(() => this.store.meta());

  constructor() {
    // Changes written or reverted elsewhere in the page show up here too.
    effect(() => {
      const revision = this.db.revision();
      if (revision !== this.seenRevision) {
        this.seenRevision = revision;
        void this.store.refresh();
      }
    });
    effect(onCleanup => {
      if (!this.autoRefresh()) {
        return;
      }
      const timer = setInterval(() => {
        if (!document.hidden && !this.store.loading()) {
          void this.store.refresh();
        }
      }, 3000);
      onCleanup(() => clearInterval(timer));
    });
  }

  ngOnInit(): void {
    if (this.store.runs().length === 0) {
      void this.store.refresh();
    }
  }

  protected refresh(): void {
    void this.store.refresh();
  }

  protected async check(): Promise<void> {
    await this.store.runCheck();
  }

  protected chooseTree(tree: StorageTree): void {
    this.store.selectedTree.set(tree.id);
    this.view.set('tree');
    if (tree.root !== null) {
      this.store.select(tree.root, { treeId: tree.id });
    }
  }

  protected clearTree(): void {
    this.store.selectedTree.set(null);
  }

  protected label(tree: StorageTree): string {
    return treeLabel(tree);
  }

  protected pages(tree: StorageTree): number {
    return treePages(tree);
  }

  protected icon(tree: StorageTree): string {
    switch (tree.kind) {
      case 'table':
        return 'table_rows';
      case 'index':
        return 'sort';
      case 'free':
        return 'recycling';
      case 'catalog':
        return 'menu_book';
      default:
        return 'account_tree';
    }
  }

  protected share(kind: (typeof KIND_ORDER)[number]): number {
    const total = this.store.usedPages();
    return total ? (this.store.totals()[kind] / total) * 100 : 0;
  }

  protected bytes(n: number): string {
    return formatBytes(n);
  }

  protected openPage(pgno: number): void {
    this.store.select(pgno);
  }

  protected metaMatches(m: { checksum: string; computedChecksum: string }): boolean {
    return m.checksum === m.computedChecksum;
  }
}
