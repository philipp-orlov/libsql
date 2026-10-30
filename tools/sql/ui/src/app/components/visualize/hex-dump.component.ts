import { CdkVirtualScrollViewport, ScrollingModule } from '@angular/cdk/scrolling';
import { ChangeDetectionStrategy, Component, computed, effect, input, output, viewChild } from '@angular/core';

/** A stretch of bytes with a meaning, painted in its own colour. */
export interface HexRegion {
  from: number;
  /** Exclusive. */
  to: number;
  label: string;
  color: string;
}

export interface HexSelection {
  from: number;
  to: number;
}

const BYTES_PER_ROW = 16;

/** A hex dump with an ASCII column, regions in colour and one highlighted range; only the visible rows are drawn. */
@Component({
  selector: 'app-hex-dump',
  imports: [ScrollingModule],
  changeDetection: ChangeDetectionStrategy.OnPush,
  templateUrl: './hex-dump.component.html',
  styleUrl: './hex-dump.component.scss',
})
export class HexDumpComponent {
  readonly bytes = input.required<Uint8Array>();
  readonly regions = input<HexRegion[]>([]);
  readonly highlight = input<HexSelection | null>(null);
  /** Added to the offsets shown (the page's place in the file). */
  readonly base = input(0);
  readonly pick = output<number>();
  private readonly viewport = viewChild(CdkVirtualScrollViewport);

  constructor() {
    // the range that is highlighted is brought into view
    effect(() => {
      const range = this.highlight();
      const viewport = this.viewport();
      if (range && viewport) {
        queueMicrotask(() => viewport.scrollToIndex(Math.max(0, Math.floor(range.from / BYTES_PER_ROW) - 2), 'smooth'));
      }
    });
  }

  protected readonly rowCount = computed(() => Math.ceil(this.bytes().length / BYTES_PER_ROW));
  protected readonly rowIndexes = computed(() => Array.from({ length: this.rowCount() }, (_, i) => i));
  protected readonly columns = Array.from({ length: BYTES_PER_ROW }, (_, i) => i);

  protected rowOffset(row: number): string {
    return (this.base() + row * BYTES_PER_ROW).toString(16).padStart(8, '0');
  }

  protected has(row: number, column: number): boolean {
    return row * BYTES_PER_ROW + column < this.bytes().length;
  }

  protected hexAt(row: number, column: number): string {
    const i = row * BYTES_PER_ROW + column;
    return i < this.bytes().length ? this.bytes()[i].toString(16).padStart(2, '0') : '  ';
  }

  protected charAt(row: number, column: number): string {
    const i = row * BYTES_PER_ROW + column;
    if (i >= this.bytes().length) {
      return ' ';
    }
    const b = this.bytes()[i];
    return b >= 0x20 && b < 0x7f ? String.fromCharCode(b) : '·';
  }

  protected colorAt(row: number, column: number): string | null {
    const i = row * BYTES_PER_ROW + column;
    for (const r of this.regions()) {
      if (i >= r.from && i < r.to) {
        return r.color;
      }
    }
    return null;
  }

  protected titleAt(row: number, column: number): string {
    const i = row * BYTES_PER_ROW + column;
    const region = this.regions().find(r => i >= r.from && i < r.to);
    return `offset ${i}${region ? ' · ' + region.label : ''}`;
  }

  protected highlighted(row: number, column: number): boolean {
    const h = this.highlight();
    const i = row * BYTES_PER_ROW + column;
    return h !== null && i >= h.from && i < h.to;
  }

  protected onPick(row: number, column: number): void {
    this.pick.emit(row * BYTES_PER_ROW + column);
  }
}
