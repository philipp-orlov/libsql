import {
  ChangeDetectionStrategy,
  Component,
  ElementRef,
  OnDestroy,
  booleanAttribute,
  computed,
  inject,
  input,
  numberAttribute,
  signal,
} from '@angular/core';

import { MatIconModule } from '@angular/material/icon';

import { SplitComponent } from './split.component';

/**
 * One pane of a {@link SplitComponent}: an optional header (icon, title, your own actions, and a Maximise button)
 * above the projected content, and, unless it is the last pane, a draggable sash on its trailing edge.
 *
 * Content slots: default (the body), `[paneTitle]` (replaces the plain title text, e.g. with tabs) and
 * `[paneActions]` (buttons at the right of the header, before Maximise).
 */
@Component({
  selector: 'ui-split-pane',
  imports: [MatIconModule],
  changeDetection: ChangeDetectionStrategy.OnPush,
  host: {
    '[class.hidden]': 'hidden()',
    '[class.maximized]': 'maximized()',
    '[class.horizontal]': "split.orientation() === 'horizontal'",
    '[style.flex]': 'flex()',
    '[style.min-width.px]': "split.orientation() === 'horizontal' && !maximized() ? minSize() : null",
    '[style.min-height.px]': "split.orientation() === 'vertical' && !maximized() ? minSize() : null",
    '(keydown.escape)': 'onEscape($event)',
  },
  template: `
    @if (!headerless()) {
      <header class="pane-header" (dblclick)="onHeaderDoubleClick($event)">
        @if (icon()) {
          <mat-icon class="pane-icon" aria-hidden="true">{{ icon() }}</mat-icon>
        }
        @if (title()) {
          <span class="pane-title">{{ title() }}</span>
        }
        <ng-content select="[paneTitle]" />
        <span class="pane-spacer"></span>
        <ng-content select="[paneActions]" />
        @if (maximizable()) {
          <button
            type="button"
            class="pane-button maximize"
            (click)="toggleMaximize()"
            [attr.title]="maximized() ? 'Restore (Esc)' : 'Maximize'"
            [attr.aria-label]="maximized() ? 'Restore ' + title() : 'Maximize ' + title()"
            [attr.aria-pressed]="maximized()"
          >
            <mat-icon aria-hidden="true">{{ maximized() ? 'close_fullscreen' : 'open_in_full' }}</mat-icon>
          </button>
        }
      </header>
    }
    <div class="pane-body"><ng-content /></div>
    @if (hasSash()) {
      <div
        class="sash"
        [class.dragging]="dragging()"
        role="separator"
        tabindex="0"
        [attr.aria-orientation]="split.orientation() === 'horizontal' ? 'vertical' : 'horizontal'"
        [attr.aria-label]="'Resize ' + (title() || 'pane')"
        (pointerdown)="onPointerDown($event)"
        (pointermove)="onPointerMove($event)"
        (pointerup)="endDrag($event)"
        (pointercancel)="endDrag($event)"
        (dblclick)="reset()"
        (keydown)="onKey($event)"
      ></div>
    }
  `,
  styleUrl: './split-pane.component.scss',
})
export class SplitPaneComponent implements OnDestroy {
  protected readonly split = inject(SplitComponent);
  readonly element = inject<ElementRef<HTMLElement>>(ElementRef).nativeElement;

  /** Header text. */
  readonly title = input('');
  /** Material icon name shown before the title. */
  readonly icon = input('');
  /** Initial size in pixels along the split axis; leave unset for the pane that takes the remaining space. */
  readonly size = input<number | undefined, unknown>(undefined, {
    transform: (v) => (v === undefined || v === null || v === '' ? undefined : numberAttribute(v)),
  });
  /** Smallest size, in pixels, the sash lets this pane shrink to. */
  readonly minSize = input(120, { transform: numberAttribute });
  /** Offers the Maximise button. */
  readonly maximizable = input(true, { transform: booleanAttribute });
  /** Draws no header at all (then there is no Maximise button either). */
  readonly headerless = input(false, { transform: booleanAttribute });

  /** The size the user dragged to; null while the pane still has its initial size. */
  readonly current = signal<number | null>(null);
  protected readonly dragging = signal(false);

  readonly maximized = computed(() => this.split.maximized() === this);
  protected readonly hidden = computed(() => {
    const max = this.split.maximized();
    return max !== null && max !== this;
  });
  protected readonly hasSash = computed(() => {
    const all = this.split.registered();
    return !this.hidden() && !this.maximized() && all.length > 1 && this.split.ordered().at(-1) !== this;
  });
  protected readonly flex = computed(() => {
    if (this.maximized()) {
      return '1 1 auto';
    }
    if (this === this.split.flexible()) {
      return '1 1 0px';
    }
    const px = this.current() ?? this.size() ?? 0;
    return `0 0 ${px}px`;
  });

  private dragStart = 0;
  private startSize = 0;

  constructor() {
    this.split.register(this);
  }

  ngOnDestroy(): void {
    this.split.unregister(this);
  }

  toggleMaximize(): void {
    this.split.toggleMaximize(this);
  }

  /** Back to the initial size. */
  reset(): void {
    this.current.set(null);
    const next = this.split.ordered()[this.split.ordered().indexOf(this) + 1];
    next?.current.set(null);
  }

  protected onHeaderDoubleClick(event: MouseEvent): void {
    if (this.maximizable() && !(event.target as HTMLElement).closest('button, input, a, mat-button-toggle-group')) {
      this.toggleMaximize();
    }
  }

  protected onEscape(event: Event): void {
    if (this.maximized()) {
      event.stopPropagation();
      this.split.restore();
    }
  }

  protected onPointerDown(event: PointerEvent): void {
    if (event.button !== 0) {
      return;
    }
    event.preventDefault();
    try {
      (event.currentTarget as HTMLElement).setPointerCapture(event.pointerId);
    } catch {
      // Not an active pointer (a synthetic event): the drag still works while the pointer stays over the sash.
    }
    this.dragging.set(true);
    this.dragStart = this.axis(event);
    this.startSize = this.split.extent(this.element);
    this.pinNeighbours();
  }

  protected onPointerMove(event: PointerEvent): void {
    if (!this.dragging()) {
      return;
    }
    this.moveTo(this.startSize + (this.axis(event) - this.dragStart));
  }

  protected endDrag(event: PointerEvent): void {
    if (this.dragging()) {
      this.dragging.set(false);
      try {
        (event.currentTarget as HTMLElement).releasePointerCapture(event.pointerId);
      } catch {
        // Nothing was captured.
      }
    }
  }

  protected onKey(event: KeyboardEvent): void {
    const horizontal = this.split.orientation() === 'horizontal';
    const step = event.shiftKey ? 64 : 16;
    let delta = 0;
    if (event.key === (horizontal ? 'ArrowLeft' : 'ArrowUp')) {
      delta = -step;
    } else if (event.key === (horizontal ? 'ArrowRight' : 'ArrowDown')) {
      delta = step;
    } else if (event.key === 'Enter') {
      this.reset();
      event.preventDefault();
      return;
    } else {
      return;
    }
    event.preventDefault();
    this.pinNeighbours();
    this.moveTo(this.split.extent(this.element) + delta);
  }

  private axis(event: PointerEvent): number {
    return this.split.orientation() === 'horizontal' ? event.clientX : event.clientY;
  }

  /** Fixes the neighbour sizes at what is on screen, so moving the sash only changes these two panes. */
  private pinNeighbours(): void {
    const panes = this.split.ordered();
    const flexible = this.split.flexible();
    for (const p of panes) {
      if (p !== flexible && p.current() === null) {
        p.current.set(this.split.extent(p.element));
      }
    }
  }

  private moveTo(size: number): void {
    const index = this.split.ordered().indexOf(this);
    this.split.moveSash(index, size - this.split.extent(this.element));
  }
}
