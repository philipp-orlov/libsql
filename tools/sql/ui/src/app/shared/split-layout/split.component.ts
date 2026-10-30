import {
  ChangeDetectionStrategy,
  Component,
  DestroyRef,
  ElementRef,
  afterNextRender,
  afterRenderEffect,
  inject,
  input,
  output,
  signal,
} from '@angular/core';

import type { SplitPaneComponent } from './split-pane.component';

export type SplitOrientation = 'horizontal' | 'vertical';

/**
 * A row (`horizontal`, panes side by side) or a column (`vertical`, panes stacked) of {@link SplitPaneComponent}s with a
 * draggable sash between neighbours, like the VS Code workbench. A pane can be maximised to fill the whole container;
 * the others stay alive but hidden, so their state survives. Self-contained: no application services, only Angular and
 * Material theme tokens, so it can be copied into other projects as is.
 *
 * Sizing: a pane with a `size` is that many pixels wide (or high) until the user drags its sash; the pane without one
 * (or else the last pane) takes the remaining space. Sizes survive the container being resized, shrinking from the end
 * first once the panes no longer fit.
 */
@Component({
  selector: 'ui-split',
  changeDetection: ChangeDetectionStrategy.OnPush,
  host: {
    '[class.horizontal]': "orientation() === 'horizontal'",
    '[class.vertical]': "orientation() === 'vertical'",
    '[class.has-maximized]': 'maximized() !== null',
  },
  template: '<ng-content />',
  styles: `
    :host {
      display: flex;
      width: 100%;
      height: 100%;
      min-width: 0;
      min-height: 0;
      overflow: hidden;
      /* the gutter the pane cards float on */
      background: var(--ui-split-background, var(--mat-sys-surface-container));
    }
    :host(.vertical) {
      flex-direction: column;
    }
  `,
})
export class SplitComponent {
  /** `horizontal` puts the panes side by side, `vertical` stacks them. */
  readonly orientation = input<SplitOrientation>('horizontal');
  /** Emits the maximised pane, or null once it is restored. */
  readonly maximizedChange = output<SplitPaneComponent | null>();

  /** The pane filling the container, if any. */
  readonly maximized = signal<SplitPaneComponent | null>(null);

  readonly host = inject<ElementRef<HTMLElement>>(ElementRef).nativeElement;
  /** Panes register themselves in template order. */
  readonly registered = signal<readonly SplitPaneComponent[]>([]);

  private readonly destroyRef = inject(DestroyRef);

  constructor() {
    // The axis changed: what fitted along the old one may overflow along the new one.
    afterRenderEffect(() => {
      this.orientation();
      this.fit();
    });
    afterNextRender(() => {
      this.fit();
      const observer = new ResizeObserver(() => this.fit());
      observer.observe(this.host);
      this.destroyRef.onDestroy(() => observer.disconnect());
    });
  }

  register(pane: SplitPaneComponent): void {
    this.registered.update((all) => [...all, pane]);
  }

  unregister(pane: SplitPaneComponent): void {
    this.registered.update((all) => all.filter((p) => p !== pane));
    if (this.maximized() === pane) {
      this.maximized.set(null);
    }
  }

  /** The pane that takes the remaining space: the one without a `size`, else the last. */
  flexible(): SplitPaneComponent | undefined {
    const all = this.registered();
    return all.find((p) => p.size() === undefined) ?? all[all.length - 1];
  }

  /** Orders the panes by their place in the DOM, so panes added by `@if`/`@for` land where they appear. */
  ordered(): SplitPaneComponent[] {
    return [...this.registered()].sort((a, b) =>
      a.element.compareDocumentPosition(b.element) & Node.DOCUMENT_POSITION_FOLLOWING ? -1 : 1,
    );
  }

  toggleMaximize(pane: SplitPaneComponent): void {
    const next = this.maximized() === pane ? null : pane;
    this.maximized.set(next);
    this.maximizedChange.emit(next);
  }

  restore(): void {
    if (this.maximized() !== null) {
      this.maximized.set(null);
      this.maximizedChange.emit(null);
    }
  }

  /** Size along the split axis of an element. */
  extent(el: HTMLElement): number {
    const box = el.getBoundingClientRect();
    return this.orientation() === 'horizontal' ? box.width : box.height;
  }

  /**
   * Moves the sash after pane `index` by `delta` pixels, taking from one neighbour and giving to the other, within
   * both minimum sizes. Panes that are not the flexible one are pinned at their current size first.
   */
  moveSash(index: number, delta: number): void {
    const panes = this.ordered();
    const before = panes[index];
    const after = panes[index + 1];
    if (!before || !after) {
      return;
    }
    const flexible = this.flexible();
    const a = this.extent(before.element);
    const total = a + this.extent(after.element);
    if (total < before.minSize() + after.minSize()) {
      return;
    }
    const size = Math.max(before.minSize(), Math.min(a + delta, total - after.minSize()));
    if (before === flexible) {
      after.current.set(total - size);
    } else {
      before.current.set(size);
      if (after !== flexible) {
        after.current.set(total - size);
      }
    }
  }

  /** Keeps the panes inside the container: when they overflow, the fixed panes give way from the last one back. */
  fit(): void {
    const panes = this.ordered();
    const flexible = this.flexible();
    if (!flexible) {
      return;
    }
    const available = this.extent(this.host);
    let fixed = 0;
    for (const p of panes) {
      if (p !== flexible) {
        fixed += p.current() ?? p.size() ?? 0;
      }
    }
    let overflow = fixed + flexible.minSize() - available;
    for (let i = panes.length - 1; i >= 0 && overflow > 0; i--) {
      const p = panes[i];
      if (p === flexible) {
        continue;
      }
      const have = p.current() ?? p.size() ?? 0;
      const give = Math.min(overflow, Math.max(0, have - p.minSize()));
      if (give > 0) {
        p.current.set(have - give);
        overflow -= give;
      }
    }
  }
}
