import { ChangeDetectionStrategy, Component, ElementRef, afterNextRender, computed, effect, inject, signal, viewChild } from '@angular/core';
import { MatIconModule } from '@angular/material/icon';
import { MatSliderModule } from '@angular/material/slider';

import { treeLabel } from '../../services/storage';
import type { PageKind } from '../../services/storage';
import { KIND_COLORS, KIND_LABELS, KIND_ORDER, VisualizeStore } from './visualize.store';

interface Cells {
  /** Pages per cell. */
  per: number;
  kinds: Uint8Array;
  trees: Int16Array;
  /** Cells that hold at least one page the check found bad. */
  bad: Uint8Array;
  count: number;
}

const GAP = 1;

/**
 * Every page of the file as a small square, in file order, coloured by what it is. When the file has more pages than
 * fit, one square stands for several pages and shows the kind most of them have. A tree selected in the list keeps its
 * colour and the rest fade; a page the check found damaged is circled in red.
 */
@Component({
  selector: 'app-page-map',
  imports: [MatIconModule, MatSliderModule],
  changeDetection: ChangeDetectionStrategy.OnPush,
  templateUrl: './page-map.component.html',
  styleUrl: './page-map.component.scss',
})
export class PageMapComponent {
  protected readonly store = inject(VisualizeStore);
  private readonly canvas = viewChild.required<ElementRef<HTMLCanvasElement>>('canvas');
  private readonly host = inject<ElementRef<HTMLElement>>(ElementRef);
  private readonly wrap = viewChild.required<ElementRef<HTMLElement>>('wrap');

  /** Side of a square in pixels. */
  protected readonly cell = signal(12);
  private readonly width = signal(800);
  protected readonly hover = signal<{ x: number; y: number; text: string[] } | null>(null);

  protected readonly kinds = KIND_ORDER;
  protected readonly colors = KIND_COLORS;
  protected readonly labels = KIND_LABELS;

  private readonly perRow = computed(() => Math.max(1, Math.floor((this.width() + GAP) / (this.cell() + GAP))));
  /** Cells the screen can reasonably hold: more pages than this share a square. */
  private readonly capacity = 40000;

  protected readonly cells = computed<Cells>(() => this.build(this.store.runs(), this.store.usedPages(), this.store.badPages()));
  protected readonly rows = computed(() => Math.ceil(this.cells().count / this.perRow()));
  protected readonly height = computed(() => Math.max(40, this.rows() * (this.cell() + GAP)));

  constructor() {
    afterNextRender(() => {
      // the room inside the frame, less its padding and a scrollbar
      const measure = () => this.width.set(Math.max(100, this.wrap().nativeElement.clientWidth - 12 - 14));
      const observer = new ResizeObserver(measure);
      observer.observe(this.wrap().nativeElement);
      measure();
    });
    effect(() => {
      // redraw when anything it shows changes
      this.cells();
      this.perRow();
      this.cell();
      this.height();
      this.store.selectedTree();
      this.store.selectedPage();
      this.draw();
    });
  }

  private build(runs: { first: number; count: number; kind: PageKind; tree: number | null }[], total: number, bad: Set<number>): Cells {
    const per = Math.max(1, Math.ceil(total / this.capacity));
    const count = Math.ceil(total / per);
    const kinds = new Uint8Array(count).fill(KIND_ORDER.indexOf('unknown'));
    const trees = new Int16Array(count).fill(-1);
    const badCells = new Uint8Array(count);
    if (per === 1) {
      for (const run of runs) {
        const kind = KIND_ORDER.indexOf(run.kind);
        for (let p = run.first; p < run.first + run.count && p < count; p++) {
          kinds[p] = kind;
          trees[p] = run.tree ?? -1;
        }
      }
    } else {
      // Dominant kind per cell: add up how many pages of each kind the runs put in it.
      const tally = new Float64Array(KIND_ORDER.length);
      let cellIndex = 0;
      let cellEnd = per;
      let owner = new Map<number, number>();
      const flush = () => {
        let best = 0;
        for (let k = 1; k < tally.length; k++) {
          if (tally[k] > tally[best]) {
            best = k;
          }
        }
        kinds[cellIndex] = best;
        let top = -1;
        let topPages = 0;
        for (const [t, n] of owner) {
          if (n > topPages) {
            top = t;
            topPages = n;
          }
        }
        trees[cellIndex] = top;
        tally.fill(0);
        owner = new Map();
      };
      for (const run of runs) {
        let at = run.first;
        const end = run.first + run.count;
        while (at < end) {
          while (at >= cellEnd) {
            flush();
            cellIndex++;
            cellEnd += per;
          }
          const take = Math.min(end, cellEnd) - at;
          tally[KIND_ORDER.indexOf(run.kind)] += take;
          if (run.tree !== null) {
            owner.set(run.tree, (owner.get(run.tree) ?? 0) + take);
          }
          at += take;
        }
      }
      flush();
    }
    for (const page of bad) {
      const c = Math.floor(page / per);
      if (c < count) {
        badCells[c] = 1;
      }
    }
    return { per, kinds, trees, bad: badCells, count };
  }

  private draw(): void {
    const element = this.canvas().nativeElement;
    const cells = this.cells();
    const size = this.cell();
    const perRow = this.perRow();
    const ratio = window.devicePixelRatio || 1;
    const w = this.width();
    const h = this.height();
    element.width = Math.floor(w * ratio);
    element.height = Math.floor(h * ratio);
    element.style.width = `${w}px`;
    element.style.height = `${h}px`;
    const ctx = element.getContext('2d');
    if (!ctx) {
      return;
    }
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    ctx.clearRect(0, 0, w, h);
    const tree = this.store.selectedTree();
    const colors = KIND_ORDER.map(k => KIND_COLORS[k]);
    for (let i = 0; i < cells.count; i++) {
      const x = (i % perRow) * (size + GAP);
      const y = Math.floor(i / perRow) * (size + GAP);
      const faded = tree !== null && cells.trees[i] !== tree && KIND_ORDER[cells.kinds[i]] !== 'meta';
      ctx.globalAlpha = faded ? 0.18 : 1;
      ctx.fillStyle = colors[cells.kinds[i]];
      ctx.fillRect(x, y, size, size);
      if (KIND_ORDER[cells.kinds[i]] === 'free') {
        // free pages are hatched: they hold stale bytes
        ctx.globalAlpha = faded ? 0.1 : 0.55;
        ctx.strokeStyle = '#ffffff';
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(x, y + size);
        ctx.lineTo(x + size, y);
        ctx.stroke();
      }
    }
    ctx.globalAlpha = 1;
    for (let i = 0; i < cells.count; i++) {
      if (cells.bad[i]) {
        const x = (i % perRow) * (size + GAP);
        const y = Math.floor(i / perRow) * (size + GAP);
        ctx.strokeStyle = '#b71c1c';
        ctx.lineWidth = 2;
        ctx.strokeRect(x + 1, y + 1, size - 2, size - 2);
      }
    }
    const selected = this.store.selectedPage();
    if (selected !== null) {
      const i = Math.floor(selected / cells.per);
      if (i < cells.count) {
        const x = (i % perRow) * (size + GAP);
        const y = Math.floor(i / perRow) * (size + GAP);
        ctx.strokeStyle = '#000000';
        ctx.lineWidth = 2;
        ctx.strokeRect(x - 1, y - 1, size + 2, size + 2);
        ctx.strokeStyle = '#ffffff';
        ctx.lineWidth = 1;
        ctx.strokeRect(x, y, size, size);
      }
    }
  }

  private cellAt(event: MouseEvent): number | null {
    const rect = this.canvas().nativeElement.getBoundingClientRect();
    const size = this.cell() + GAP;
    const col = Math.floor((event.clientX - rect.left) / size);
    const row = Math.floor((event.clientY - rect.top) / size);
    if (col < 0 || row < 0 || col >= this.perRow()) {
      return null;
    }
    const i = row * this.perRow() + col;
    return i < this.cells().count ? i : null;
  }

  protected onMove(event: MouseEvent): void {
    const i = this.cellAt(event);
    if (i === null) {
      this.hover.set(null);
      return;
    }
    const cells = this.cells();
    const first = i * cells.per;
    const last = Math.min(this.store.usedPages(), first + cells.per) - 1;
    const kind = KIND_ORDER[cells.kinds[i]];
    const tree = cells.trees[i] >= 0 ? this.store.trees().find(t => t.id === cells.trees[i]) : null;
    const lines = [cells.per === 1 ? `Page ${first}` : `Pages ${first}–${last}`, KIND_LABELS[kind] + (cells.per > 1 ? ' (mostly)' : '')];
    if (tree) {
      lines.push(`${tree.kind === 'index' ? 'index' : tree.kind === 'table' ? 'table' : 'tree'} ${treeLabel(tree)}`);
    }
    if (cells.bad[i]) {
      lines.push('failed the integrity check');
    }
    const rect = this.host.nativeElement.getBoundingClientRect();
    this.hover.set({ x: event.clientX - rect.left + 14, y: event.clientY - rect.top + 14, text: lines });
  }

  protected onClick(event: MouseEvent): void {
    const i = this.cellAt(event);
    if (i === null) {
      return;
    }
    const cells = this.cells();
    let page = i * cells.per;
    if (cells.per > 1) {
      // a square for many pages opens the first page of the kind it shows
      const kind = KIND_ORDER[cells.kinds[i]];
      const hit = this.store.runs().find(r => r.kind === kind && r.first + r.count > page && r.first < page + cells.per);
      if (hit) {
        page = Math.max(hit.first, page);
      }
    }
    this.store.select(page);
  }

  protected setCell(value: number): void {
    this.cell.set(value);
  }

  protected percent(kind: PageKind): string {
    const total = this.store.usedPages();
    return total ? ((this.store.totals()[kind] / total) * 100).toFixed(1) + '%' : '';
  }
}
