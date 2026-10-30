// Copyright (c) 2026 Philipp Orlov

import { Component, ElementRef, computed, effect, input, model, output, signal, untracked, viewChild } from '@angular/core';
import { MatInputModule } from '@angular/material/input';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';

import { Action, PageDoc, pageToSvg, Region, Tip, ViewState } from '../core';
import { Choice, SelectComponent } from '../shared/select.component';
import { FONT_FAMILY } from '../report.service';

/** An item of the report the reader pointed at in the preview (the designer selects it for editing). */
export interface ItemPick {
  section: Region['section'];
  index: number;
}

/**
 * The pages of a laid-out report, one at a time, drawn from the same primitives the PDF is made of, and alive: the pointer
 * shows a card with the numbers of a bar, point or slice, marks of one category highlight together, a click selects a
 * category (in every chart at once), the legend switches series on and off, a table's headings sort it and its groups fold.
 * What the reader changes is the `view` state: whoever owns the report lays it out again with it, so the PDF follows.
 */
@Component({
  selector: 'app-report-viewer',
  imports: [MatInputModule, MatButtonModule, MatIconModule, SelectComponent],
  templateUrl: './report-viewer.component.html',
  styleUrl: './report-viewer.component.scss',
  host: { '(keydown)': 'onKey($event)', tabindex: '0' },
})
export class ReportViewerComponent {
  readonly pages = input<PageDoc[]>([]);
  readonly busy = input(false);
  /** What the reader changed: series switched off, a sort, folded groups. */
  readonly view = model<ViewState>({});
  /** In the designer: a click on an item selects it for editing instead of selecting data. */
  readonly pickable = input(false);
  readonly picked = input<ItemPick | null>(null);
  readonly pick = output<ItemPick>();

  protected readonly index = signal(0);
  protected readonly zoom = signal<'fit' | number>('fit');
  protected readonly zoomOptions: Choice[] = [
    { value: 'fit', label: 'Fit width' },
    ...[50, 75, 100, 150, 200].map(z => ({ value: String(z), label: `${z}%` })),
  ];
  protected readonly zoomKey = computed(() => (this.zoom() === 'fit' ? 'fit' : String(Math.round((this.zoom() as number) * 100))));
  protected readonly count = computed(() => this.pages().length);
  protected readonly current = computed(() => Math.min(this.index(), Math.max(this.count() - 1, 0)));
  protected readonly selected = signal<string | null>(null);
  protected readonly changed = computed(() => {
    const v = this.view();
    return !!(Object.values(v.hidden ?? {}).some(h => h.length) || Object.keys(v.sort ?? {}).length || v.folded?.length) || this.selected() !== null;
  });

  private readonly sheet = viewChild<ElementRef<HTMLElement>>('sheet');
  /** Where the SVG of the page is put (the overlays beside it must survive a redraw). */
  private readonly canvas = viewChild<ElementRef<HTMLElement>>('canvas');
  private readonly tip = viewChild<ElementRef<HTMLElement>>('tip');
  private readonly outline = viewChild<ElementRef<HTMLElement>>('outline');
  private marks: Element[] = [];
  private hover: string | null = null;

  // The SVG is made here from text the SVG writer escapes, never from markup that came with the data.
  private readonly svg = computed(() => {
    const page = this.pages()[this.current()];
    return page ? pageToSvg(page, FONT_FAMILY) : '';
  });

  protected readonly width = computed(() => {
    const page = this.pages()[this.current()];
    const z = this.zoom();
    return page && z !== 'fit' ? `${Math.round(page.width * (96 / 72) * z)}px` : '100%';
  });

  constructor() {
    // Draw the page, and give what is drawn its listeners and the state it had (selection, the picked item).
    effect(() => {
      const el = this.canvas()?.nativeElement;
      const svg = this.svg();
      if (!el) return;
      untracked(() => {
        el.innerHTML = svg;
        this.marks = Array.from(el.querySelectorAll('.bar,.point,.slice'));
        this.hideTip();
        this.focus(this.selected());
        this.showPicked();
      });
    });
    effect(() => {
      this.picked();
      this.pages();
      this.index();
      untracked(() => this.showPicked());
    });
    // Another report (or none yet) starts at its first page, with nothing selected.
    effect(() => {
      if (this.pages().length === 0) {
        untracked(() => {
          this.index.set(0);
          this.selected.set(null);
        });
      }
    });
  }

  // ---- paging ---------------------------------------------------------------------------------------------------------

  protected go(delta: number): void {
    this.index.set(Math.min(Math.max(this.current() + delta, 0), this.count() - 1));
  }

  protected jump(value: string): void {
    const n = Number.parseInt(value, 10);
    if (Number.isFinite(n)) this.index.set(Math.min(Math.max(n - 1, 0), this.count() - 1));
  }

  protected setZoom(value: string): void {
    this.zoom.set(value === 'fit' ? 'fit' : Number(value) / 100);
  }

  protected onKey(event: KeyboardEvent): void {
    if (event.target instanceof HTMLInputElement || event.target instanceof HTMLSelectElement) return;
    if (event.key === 'Escape') this.reset();
    else if (event.key === 'PageDown' || event.key === 'ArrowRight') this.go(1);
    else if (event.key === 'PageUp' || event.key === 'ArrowLeft') this.go(-1);
    else if (event.key === 'Home') this.index.set(0);
    else if (event.key === 'End') this.index.set(Math.max(this.count() - 1, 0));
    else return;
    event.preventDefault();
  }

  // ---- the pointer ----------------------------------------------------------------------------------------------------

  protected onMove(event: PointerEvent): void {
    const target = event.target as Element;
    const mark = target.closest('[data-tip],[data-k],[data-act]');
    const tip = mark?.getAttribute('data-tip');
    if (tip) this.showTip(JSON.parse(tip) as Tip, event);
    else this.hideTip();
    this.hover = mark?.getAttribute('data-k') ?? null;
    this.focus(this.hover ?? this.selected());
    if (this.pickable()) this.outlineAt(event, !mark?.getAttribute('data-act'));
  }

  protected onLeave(): void {
    this.hideTip();
    this.hover = null;
    this.focus(this.selected());
    this.outlineHide();
  }

  protected onClick(event: MouseEvent): void {
    const mark = (event.target as Element).closest('[data-act]');
    const act = mark?.getAttribute('data-act');
    if (act) {
      this.apply(JSON.parse(act) as Action);
      return;
    }
    if (this.pickable()) {
      const region = this.regionAt(event);
      if (region) this.pick.emit({ section: region.section, index: region.index });
      return;
    }
    const key = (event.target as Element).closest('[data-k]')?.getAttribute('data-k') ?? null;
    this.selected.set(key !== null && key !== this.selected() ? key : null);
    this.focus(this.selected());
  }

  protected reset(): void {
    this.view.set({});
    this.selected.set(null);
    this.focus(null);
  }

  private apply(a: Action): void {
    const v = structuredClone(this.view());
    if (a.type === 'series') {
      const hidden = (v.hidden ??= {});
      const list = (hidden[String(a.item)] ??= []);
      const at = list.indexOf(a.index);
      if (at >= 0) list.splice(at, 1);
      else list.push(a.index);
    } else if (a.type === 'sort') {
      const sort = (v.sort ??= {});
      const cur = sort[String(a.item)];
      if (!cur || cur.column !== a.column) sort[String(a.item)] = { column: a.column, dir: 'asc' };
      else if (cur.dir === 'asc') cur.dir = 'desc';
      else delete sort[String(a.item)];
    } else {
      const folded = (v.folded ??= []);
      const at = folded.indexOf(a.id);
      if (at >= 0) folded.splice(at, 1);
      else folded.push(a.id);
    }
    this.view.set(v);
  }

  /** Marks of the category stay, the others fade. */
  private focus(key: string | null): void {
    const svg = this.canvas()?.nativeElement.firstElementChild;
    svg?.classList.toggle('focus', key !== null);
    for (const el of this.marks) el.classList.toggle('hl', key !== null && el.getAttribute('data-k') === key);
  }

  private showTip(tip: Tip, event: PointerEvent): void {
    const card = this.tip()?.nativeElement;
    const stage = this.sheet()?.nativeElement.parentElement;
    if (!card || !stage) return;
    card.replaceChildren();
    if (tip.title) {
      const t = document.createElement('div');
      t.className = 't';
      t.textContent = tip.title;
      card.append(t);
    }
    for (const row of tip.rows) {
      const r = document.createElement('div');
      r.className = 'r';
      const dot = document.createElement('i');
      if (row.color) dot.style.background = row.color;
      else dot.style.visibility = 'hidden';
      const label = document.createElement('span');
      label.textContent = row.label;
      const value = document.createElement('b');
      value.textContent = row.value;
      if (row.bold) r.classList.add('bold');
      r.append(dot, label, value);
      card.append(r);
    }
    card.hidden = false;
    const box = stage.getBoundingClientRect();
    const w = card.offsetWidth;
    const h = card.offsetHeight;
    let x = event.clientX - box.left + stage.scrollLeft + 14;
    let y = event.clientY - box.top + stage.scrollTop + 14;
    if (x + w > stage.scrollLeft + box.width - 8) x = event.clientX - box.left + stage.scrollLeft - w - 14;
    if (y + h > stage.scrollTop + box.height - 8) y = event.clientY - box.top + stage.scrollTop - h - 14;
    card.style.transform = `translate(${Math.max(x, 4)}px, ${Math.max(y, 4)}px)`;
  }

  private hideTip(): void {
    const card = this.tip()?.nativeElement;
    if (card) card.hidden = true;
  }

  // ---- picking an item (the designer) --------------------------------------------------------------------------------

  private scale(): number {
    const svg = this.canvas()?.nativeElement.firstElementChild as SVGElement | null;
    const page = this.pages()[this.current()];
    return svg && page ? svg.getBoundingClientRect().width / page.width : 1;
  }

  private regionAt(event: MouseEvent): Region | undefined {
    const svg = this.canvas()?.nativeElement.firstElementChild;
    const page = this.pages()[this.current()];
    if (!svg || !page) return undefined;
    const box = svg.getBoundingClientRect();
    const k = this.scale();
    const x = (event.clientX - box.left) / k;
    const y = (event.clientY - box.top) / k;
    return (page.regions ?? []).find(r => x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h);
  }

  private place(el: HTMLElement, r: Region): void {
    const svg = this.canvas()!.nativeElement.firstElementChild!;
    const host = this.sheet()!.nativeElement;
    const k = this.scale();
    const off = svg.getBoundingClientRect();
    const base = host.getBoundingClientRect();
    el.style.left = `${off.left - base.left + r.x * k - 2}px`;
    el.style.top = `${off.top - base.top + r.y * k - 2}px`;
    el.style.width = `${r.w * k + 4}px`;
    el.style.height = `${r.h * k + 4}px`;
    el.hidden = false;
  }

  private outlineAt(event: MouseEvent, show: boolean): void {
    const el = this.outline()?.nativeElement;
    if (!el) return;
    const region = show ? this.regionAt(event) : undefined;
    if (region) this.place(el, region);
    else el.hidden = true;
  }

  private outlineHide(): void {
    const el = this.outline()?.nativeElement;
    if (el) el.hidden = true;
  }

  /** The outline of the item the designer is editing stays on it. */
  private showPicked(): void {
    const host = this.sheet()?.nativeElement;
    const mark = host?.querySelector<HTMLElement>('.picked');
    if (!host || !mark) return;
    const picked = this.picked();
    const page = this.pages()[this.current()];
    const region = picked && page ? (page.regions ?? []).find(r => r.section === picked.section && r.index === picked.index) : undefined;
    if (region) this.place(mark, region);
    else mark.hidden = true;
  }
}
