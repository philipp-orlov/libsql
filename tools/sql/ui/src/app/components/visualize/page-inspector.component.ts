import { ScrollingModule } from '@angular/cdk/scrolling';
import { ChangeDetectionStrategy, Component, computed, effect, inject, signal, untracked } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatButtonToggleModule } from '@angular/material/button-toggle';
import { MatIconModule } from '@angular/material/icon';
import { MatProgressBarModule } from '@angular/material/progress-bar';
import { MatTabsModule } from '@angular/material/tabs';
import { MatTooltipModule } from '@angular/material/tooltip';

import { DatabaseService } from '../../services/database.service';
import { PageDetails, PageNode, flagNames, formatBytes, loadBytes, loadNodes, loadPage, preview, toHex, treeLabel } from '../../services/storage';
import { HexDumpComponent, HexRegion, HexSelection } from './hex-dump.component';
import { KIND_COLORS, KIND_LABELS, VisualizeStore } from './visualize.store';

export interface Segment {
  label: string;
  bytes: number;
  color: string;
  detail: string;
}

const PART_COLORS = {
  header: '#9575cd',
  slots: '#26a69a',
  free: '#e0e0e0',
  nodes: '#64b5f6',
  shared: '#ffb74d',
  hash: '#f06292',
  data: '#ffcc80',
};

type KeyMode = 'auto' | 'hex';

/**
 * One page, taken apart: its header, how its bytes are used, the stored hash against the one computed, every node on
 * it (keys, values or child pages) and the raw bytes with each part coloured. Links go to the child page or to the
 * overflow run a large value lives on.
 */
@Component({
  selector: 'app-page-inspector',
  imports: [HexDumpComponent, MatButtonModule, MatButtonToggleModule, MatIconModule, MatProgressBarModule, MatTabsModule, MatTooltipModule, ScrollingModule],
  changeDetection: ChangeDetectionStrategy.OnPush,
  templateUrl: './page-inspector.component.html',
  styleUrl: './page-inspector.component.scss',
})
export class PageInspectorComponent {
  protected readonly store = inject(VisualizeStore);
  private readonly db = inject(DatabaseService);

  protected readonly details = signal<PageDetails | null>(null);
  protected readonly nodes = signal<PageNode[]>([]);
  protected readonly bytes = signal<Uint8Array>(new Uint8Array(0));
  protected readonly bytesPages = signal(1);
  protected readonly loading = signal(false);
  protected readonly error = signal<string | null>(null);
  protected readonly selectedNode = signal<number | null>(null);
  protected readonly keyMode = signal<KeyMode>('auto');
  protected readonly tab = signal(0);

  protected readonly labels = KIND_LABELS;
  protected readonly colors = KIND_COLORS;
  protected readonly partColors = PART_COLORS;

  private ticket = 0;

  protected readonly owner = computed(() => {
    const page = this.store.selectedPage();
    return page === null ? null : this.store.ownerOf(page);
  });
  protected readonly treeName = computed(() => {
    const tree = this.owner()?.tree;
    return tree ? treeLabel(tree) : null;
  });

  protected readonly flags = computed(() => flagNames(this.details()?.flags ?? 0));
  protected readonly isTreePage = computed(() => {
    const kind = this.details()?.kind;
    return kind === 'branch' || kind === 'leaf';
  });
  protected readonly isBranch = computed(() => this.details()?.kind === 'branch');
  protected readonly sizeOfPage = computed(() => this.details()?.pageSize ?? 0);

  /** How the page's bytes are used, in file order. */
  protected readonly segments = computed<Segment[]>(() => {
    const d = this.details();
    if (!d) {
      return [];
    }
    if (d.kind === 'overflow') {
      const total = d.runPages * d.pageSize;
      return [
        { label: 'Header', bytes: 16, color: PART_COLORS.header, detail: '16 bytes: page number, flags, run length' },
        { label: 'Value', bytes: total - 24, color: PART_COLORS.data, detail: `${formatBytes(d.runBytes)} of the run can hold value bytes` },
        { label: 'Hash', bytes: 8, color: PART_COLORS.hash, detail: '8 bytes, XXH3-64 over the rest of the run' },
      ];
    }
    if (d.kind === 'meta') {
      return [
        { label: 'Header', bytes: 16, color: PART_COLORS.header, detail: '16 bytes' },
        { label: 'Meta record', bytes: 968, color: PART_COLORS.nodes, detail: '968 bytes (see the Meta view)' },
        { label: 'Unused', bytes: d.pageSize - 984, color: PART_COLORS.free, detail: 'the rest of the page' },
      ];
    }
    if (!d.headerOk) {
      return [{ label: 'Page', bytes: d.pageSize, color: PART_COLORS.free, detail: 'the header does not describe a usable page' }];
    }
    const top = d.pageSize - 8;
    const shared = d.sharedBlock;
    return [
      { label: 'Header', bytes: 16, color: PART_COLORS.header, detail: '16 bytes' },
      { label: 'Slots', bytes: d.lower - 16, color: PART_COLORS.slots, detail: `${d.nkeys} slots × 2 bytes, each the offset of a node` },
      { label: 'Free', bytes: d.upper - d.lower, color: PART_COLORS.free, detail: `${d.freeBytes} bytes between the slots and the nodes` },
      { label: 'Nodes', bytes: top - shared - d.upper, color: PART_COLORS.nodes, detail: `${top - shared - d.upper} bytes of nodes, packed from the end of the page downwards` },
      ...(shared ? [{ label: d.suffixKeys ? 'Shared suffix' : 'Shared prefix', bytes: shared, color: PART_COLORS.shared, detail: `${d.sharedBytes} bytes every key on the page shares, stored once` }] : []),
      { label: 'Hash', bytes: 8, color: PART_COLORS.hash, detail: '8 bytes, XXH3-64 over the rest of the page' },
    ].filter(s => s.bytes > 0);
  });

  protected readonly regions = computed<HexRegion[]>(() => {
    const out: HexRegion[] = [];
    let at = 0;
    for (const s of this.segments()) {
      out.push({ from: at, to: at + s.bytes, label: s.label, color: this.tint(s.color) });
      at += s.bytes;
    }
    // an overflow run shows only its first pages: the hash is not among them
    const shown = this.bytes().length;
    return out.filter(r => r.from < shown).map(r => ({ ...r, to: Math.min(r.to, shown) }));
  });

  protected readonly highlight = computed<HexSelection | null>(() => {
    const node = this.nodes().find(n => n.index === this.selectedNode());
    return node ? { from: node.offset, to: node.offset + node.size } : null;
  });

  protected readonly current = computed(() => this.nodes().find(n => n.index === this.selectedNode()) ?? null);

  protected readonly fillPercent = computed(() => {
    const d = this.details();
    return d && d.pageSize ? Math.round((d.usedBytes / d.pageSize) * 100) : 0;
  });

  constructor() {
    effect(() => {
      const page = this.store.selectedPage();
      // the snapshot changes when it is refreshed
      this.store.takenAt();
      untracked(() => void this.load(page));
    });
  }

  private tint(color: string): string {
    // the segment colours as translucent washes, so the digits stay readable
    const n = parseInt(color.slice(1), 16);
    return `rgba(${(n >> 16) & 255}, ${(n >> 8) & 255}, ${n & 255}, 0.38)`;
  }

  private async load(page: number | null): Promise<void> {
    const ticket = ++this.ticket;
    this.error.set(null);
    this.selectedNode.set(null);
    if (page === null) {
      this.details.set(null);
      this.nodes.set([]);
      this.bytes.set(new Uint8Array(0));
      return;
    }
    this.loading.set(true);
    try {
      const conn = this.db.conn;
      const details = await loadPage(conn, page);
      if (ticket !== this.ticket) {
        return;
      }
      const kind = this.store.ownerOf(page)?.tree?.kind ?? null;
      const wantsNodes = details.kind === 'branch' || details.kind === 'leaf';
      const [nodes, raw] = await Promise.all([
        wantsNodes && details.headerOk ? loadNodes(conn, page, kind).catch(() => [] as PageNode[]) : Promise.resolve([] as PageNode[]),
        loadBytes(conn, page).catch(() => ({ bytes: new Uint8Array(0), pages: 1 })),
      ]);
      if (ticket !== this.ticket) {
        return;
      }
      this.details.set(details);
      this.nodes.set(nodes);
      this.bytes.set(raw.bytes);
      this.bytesPages.set(raw.pages);
    } catch (failure) {
      if (ticket === this.ticket) {
        this.details.set(null);
        this.nodes.set([]);
        this.error.set(failure instanceof Error ? failure.message : String(failure));
      }
    } finally {
      if (ticket === this.ticket) {
        this.loading.set(false);
      }
    }
  }

  // ------------------------------------------------------------------------------------------- display helpers ---

  protected percent(bytes: number): number {
    const total = this.sizeOfPage() * (this.details()?.kind === 'overflow' ? this.details()!.runPages : 1);
    return total ? (bytes / total) * 100 : 0;
  }

  protected keyOf(node: PageNode): string {
    if (this.isBranch() && node.index === 0) {
      return '−∞';
    }
    if (this.keyMode() === 'hex') {
      return toHex(node.key, 24);
    }
    if (node.rowid !== null) {
      return `rowid ${node.rowid}`;
    }
    if (node.keyText !== null) {
      return node.keyText;
    }
    return preview(node.key, node.keyBytes);
  }

  protected valueOf(node: PageNode): string {
    if (this.keyMode() === 'hex') {
      return toHex(node.value, 24);
    }
    if (node.rowText !== null) {
      return node.rowText;
    }
    if (node.subdb) {
      return 'sub-database descriptor';
    }
    return preview(node.value, node.valueBytes, 80);
  }

  protected childShare(node: PageNode): number {
    const total = this.details()?.entries ?? 0;
    return total ? (node.count / total) * 100 : 0;
  }

  protected open(pgno: number): void {
    this.store.select(pgno);
  }

  protected go(value: string): void {
    const n = Number(value.trim());
    if (Number.isInteger(n) && n >= 0 && n < this.store.usedPages()) {
      this.store.select(n);
    }
  }

  protected step(delta: number): void {
    const page = this.store.selectedPage();
    if (page !== null) {
      const next = page + delta;
      if (next >= 0 && next < this.store.usedPages()) {
        this.store.select(next);
      }
    }
  }

  protected chooseNode(node: PageNode): void {
    this.selectedNode.set(this.selectedNode() === node.index ? null : node.index);
  }

  protected pickByte(offset: number): void {
    const node = this.nodes().find(n => offset >= n.offset && offset < n.offset + n.size);
    if (node) {
      this.selectedNode.set(node.index);
    }
  }

  protected hex(bytes: Uint8Array): string {
    return toHex(bytes, 64);
  }

  protected bytesText(n: number): string {
    return formatBytes(n);
  }

  protected setTab(index: number): void {
    this.tab.set(index);
  }

  protected trackNode(_: number, node: PageNode): number {
    return node.index;
  }
}
