import { ChangeDetectionStrategy, Component, ElementRef, computed, effect, inject, signal, untracked, viewChild } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatButtonToggleModule } from '@angular/material/button-toggle';
import { MatIconModule } from '@angular/material/icon';
import { MatTooltipModule } from '@angular/material/tooltip';

import { DatabaseService } from '../../services/database.service';
import { ChildSummary, PageNode, formatBytes, loadChildren, loadNodes, loadPage, preview, treeLabel } from '../../services/storage';
import { KIND_COLORS, KIND_LABELS, VisualizeStore } from './visualize.store';

/** A page of the tree as the explorer knows it. */
export interface ExplorerNode {
  pgno: number;
  parent: number | null;
  level: number;
  summary: ChildSummary | null;
  /** The separator key the parent files this page under (its lowest key, roughly). */
  separator: string;
  /** Entries below, as the parent counts them. */
  count: number;
  children: number[] | null;
  loading: boolean;
}

interface Row {
  node: ExplorerNode;
  expanded: boolean;
}

interface Box {
  node: ExplorerNode;
  x: number;
  y: number;
}

interface Edge {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
  to: number;
}

const BOX_W = 74;
const BOX_H = 46;
const GAP_X = 12;
const GAP_Y = 52;
const MAX_LOADED_PER_CLICK = 300;

/**
 * A tree's pages, opened one level at a time: the root, its children and so on down to the leaves, each with how full it
 * is, how many entries lie below it and the separator key it is filed under. Shown as an indented list or as a diagram
 * of the opened part. A click selects the page for the inspector.
 */
@Component({
  selector: 'app-tree-explorer',
  imports: [MatButtonModule, MatButtonToggleModule, MatIconModule, MatTooltipModule],
  changeDetection: ChangeDetectionStrategy.OnPush,
  templateUrl: './tree-explorer.component.html',
  styleUrl: './tree-explorer.component.scss',
})
export class TreeExplorerComponent {
  protected readonly store = inject(VisualizeStore);
  private readonly db = inject(DatabaseService);

  protected readonly nodes = signal<ReadonlyMap<number, ExplorerNode>>(new Map());
  protected readonly expanded = signal<ReadonlySet<number>>(new Set());
  protected readonly view = signal<'list' | 'diagram'>('list');
  /** The diagram scaled to the width of the pane, or at its real size. */
  protected readonly fit = signal(false);
  private readonly wrap = viewChild<ElementRef<HTMLElement>>('wrap');
  protected readonly message = signal<string | null>(null);
  protected readonly colors = KIND_COLORS;
  protected readonly labels = KIND_LABELS;
  protected readonly boxW = BOX_W;
  protected readonly boxH = BOX_H;

  protected readonly tree = this.store.selectedTreeInfo;
  protected readonly title = computed(() => {
    const t = this.tree();
    return t ? treeLabel(t) : '';
  });
  private ticket = 0;

  /** The rows of the list: opened pages, depth first. */
  protected readonly rows = computed<Row[]>(() => {
    const tree = this.tree();
    const nodes = this.nodes();
    const open = this.expanded();
    const out: Row[] = [];
    if (!tree || tree.root === null) {
      return out;
    }
    const visit = (pgno: number) => {
      const node = nodes.get(pgno);
      if (!node) {
        return;
      }
      const isOpen = open.has(pgno);
      out.push({ node, expanded: isOpen });
      if (isOpen && node.children) {
        node.children.forEach(visit);
      }
    };
    visit(tree.root);
    return out;
  });

  /** The diagram: opened pages in levels, parents centred over their children. */
  protected readonly diagram = computed(() => {
    const tree = this.tree();
    const nodes = this.nodes();
    const open = this.expanded();
    const boxes: Box[] = [];
    const edges: Edge[] = [];
    if (!tree || tree.root === null || !nodes.has(tree.root)) {
      return { boxes, edges, width: 0, height: 0 };
    }
    let column = 0;
    let deepest = 0;
    const place = (pgno: number, level: number): number => {
      const node = nodes.get(pgno)!;
      deepest = Math.max(deepest, level);
      const kids = open.has(pgno) && node.children ? node.children.filter(c => nodes.has(c)) : [];
      let x: number;
      if (kids.length === 0) {
        x = column++ * (BOX_W + GAP_X);
      } else {
        const xs = kids.map(c => place(c, level + 1));
        x = (xs[0] + xs[xs.length - 1]) / 2;
        const y = level * (BOX_H + GAP_Y);
        for (let i = 0; i < kids.length; i++) {
          edges.push({ x1: x + BOX_W / 2, y1: y + BOX_H, x2: xs[i] + BOX_W / 2, y2: (level + 1) * (BOX_H + GAP_Y), to: kids[i] });
        }
      }
      boxes.push({ node, x, y: level * (BOX_H + GAP_Y) });
      return x;
    };
    place(tree.root, 0);
    return { boxes, edges, width: Math.max(1, column) * (BOX_W + GAP_X), height: (deepest + 1) * (BOX_H + GAP_Y) - GAP_Y + 2 };
  });

  protected readonly total = computed(() => this.tree()?.entries ?? 0);

  constructor() {
    effect(() => {
      // at real size the root is kept in view: it sits over the middle of everything below it
      const wrap = this.wrap()?.nativeElement;
      const d = this.diagram();
      const tree = this.tree();
      if (wrap && !this.fit() && tree?.root != null) {
        const root = d.boxes.find(b => b.node.pgno === tree.root);
        if (root) {
          queueMicrotask(() => (wrap.scrollLeft = Math.max(0, root.x + BOX_W / 2 - wrap.clientWidth / 2)));
        }
      }
    });
    effect(() => {
      const tree = this.tree();
      this.store.takenAt();
      untracked(() => void this.reset(tree?.id ?? null));
    });
  }

  private async reset(treeId: number | null): Promise<void> {
    const ticket = ++this.ticket;
    this.nodes.set(new Map());
    this.expanded.set(new Set());
    this.message.set(null);
    const tree = this.store.trees().find(t => t.id === treeId);
    if (!tree || tree.root === null) {
      return;
    }
    try {
      const root = await loadPage(this.db.conn, tree.root);
      if (ticket !== this.ticket) {
        return;
      }
      const node: ExplorerNode = {
        pgno: root.pgno,
        parent: null,
        level: 0,
        summary: { pgno: root.pgno, kind: root.kind, nkeys: root.nkeys, freeBytes: root.freeBytes, usedBytes: root.usedBytes, pageSize: root.pageSize, entries: root.entries, hashOk: root.hashOk, headerOk: root.headerOk },
        separator: '',
        count: tree.entries,
        children: null,
        loading: false,
      };
      this.nodes.set(new Map([[node.pgno, node]]));
      if (root.kind === 'branch') {
        await this.open(root.pgno);
      }
    } catch (failure) {
      this.message.set(failure instanceof Error ? failure.message : String(failure));
    }
  }

  private put(node: ExplorerNode): void {
    this.nodes.update(map => new Map(map).set(node.pgno, node));
  }

  /** Loads a branch page's children (once) and shows them. Returns how many pages it loaded. */
  protected async open(pgno: number): Promise<number> {
    const ticket = this.ticket;
    const node = this.nodes().get(pgno);
    if (!node || node.summary?.kind !== 'branch') {
      return 0;
    }
    let loaded = 0;
    if (!node.children) {
      this.put({ ...node, loading: true });
      try {
        const tree = this.tree();
        const kind = tree?.kind ?? null;
        const [items, summaries] = await Promise.all([loadNodes(this.db.conn, pgno, kind), loadChildren(this.db.conn, pgno).catch(() => [] as ChildSummary[])]);
        if (ticket !== this.ticket) {
          return 0;
        }
        const byPage = new Map(summaries.map(s => [s.pgno, s]));
        const children = items.map(i => i.child);
        const next = new Map(this.nodes());
        items.forEach(item => {
          if (!next.has(item.child)) {
            next.set(item.child, {
              pgno: item.child,
              parent: pgno,
              level: node.level + 1,
              summary: byPage.get(item.child) ?? null,
              separator: this.separatorOf(item),
              count: item.count,
              children: null,
              loading: false,
            });
            loaded++;
          }
        });
        next.set(pgno, { ...node, children, loading: false });
        this.nodes.set(next);
      } catch (failure) {
        this.put({ ...node, loading: false });
        this.message.set(failure instanceof Error ? failure.message : String(failure));
        return 0;
      }
    }
    this.expanded.update(open => new Set(open).add(pgno));
    return loaded;
  }

  private separatorOf(item: PageNode): string {
    if (item.index === 0) {
      return '−∞';
    }
    if (item.rowid !== null) {
      return `rowid ≥ ${item.rowid}`;
    }
    if (item.keyText !== null) {
      return `≥ ${item.keyText}`;
    }
    return '≥ ' + preview(item.key, item.keyBytes, 28);
  }

  protected close(pgno: number): void {
    this.expanded.update(open => {
      const next = new Set(open);
      next.delete(pgno);
      return next;
    });
  }

  protected async toggle(node: ExplorerNode, event?: Event): Promise<void> {
    event?.stopPropagation();
    if (this.expanded().has(node.pgno)) {
      this.close(node.pgno);
    } else {
      await this.open(node.pgno);
    }
  }

  /** Opens every branch that shows now, one more level; stops at a limit so that a huge tree is not read whole. */
  protected async expandLevel(): Promise<void> {
    this.message.set(null);
    const targets = this.rows()
      .filter(r => r.node.summary?.kind === 'branch' && !r.expanded)
      .map(r => r.node.pgno);
    let loaded = 0;
    for (const pgno of targets) {
      loaded += await this.open(pgno);
      if (loaded >= MAX_LOADED_PER_CLICK) {
        this.message.set(`Stopped after ${loaded} pages; press again to open more.`);
        break;
      }
    }
  }

  protected collapseAll(): void {
    const tree = this.tree();
    this.expanded.set(tree?.root != null ? new Set([tree.root]) : new Set());
  }

  protected select(node: ExplorerNode): void {
    this.store.select(node.pgno, { treeId: this.tree()?.id ?? null });
  }

  protected fill(node: ExplorerNode): number {
    const s = node.summary;
    return s && s.pageSize ? Math.round((s.usedBytes / s.pageSize) * 100) : 0;
  }

  protected share(node: ExplorerNode): number {
    const total = this.total();
    return total ? Math.min(100, (node.count / total) * 100) : 0;
  }

  protected kindOf(node: ExplorerNode): 'branch' | 'leaf' | 'unknown' {
    const kind = node.summary?.kind;
    return kind === 'branch' || kind === 'leaf' ? kind : 'unknown';
  }

  protected bytes(n: number): string {
    return formatBytes(n);
  }

  protected boxFill(node: ExplorerNode): number {
    return this.fill(node) / 100;
  }

  protected trackRow(_: number, row: Row): number {
    return row.node.pgno;
  }

  protected trackBox(_: number, box: Box): number {
    return box.node.pgno;
  }
}
