import { NgTemplateOutlet } from '@angular/common';
import {
  ChangeDetectionStrategy,
  Component,
  DestroyRef,
  ElementRef,
  computed,
  contentChild,
  inject,
  input,
  linkedSignal,
  model,
  output,
  signal,
} from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatChipsModule } from '@angular/material/chips';
import { MatDividerModule } from '@angular/material/divider';
import { MatIconModule } from '@angular/material/icon';
import { MatMenuModule } from '@angular/material/menu';
import { MatPaginatorModule, PageEvent } from '@angular/material/paginator';
import { MatTableModule } from '@angular/material/table';

import { GridCellDirective, GridGroupHeaderDirective } from './data-grid.directives';
import {
  CellRef,
  GridCellEvent,
  GridColumn,
  GridDataRow,
  GridState,
  GridViewRow,
  GroupDescriptor,
  SortDescriptor,
  SortDirection,
} from './data-grid.models';
import { buildView, columnText, columnTitle, columnValue, effectiveSort, filterItems, moveItem, sortItems } from './data-grid.process';

const DRAG_THRESHOLD_PX = 5;
const MIN_COLUMN_WIDTH_PX = 48;

interface DragSource {
  kind: 'column' | 'group';
  field: string;
  title: string;
}

interface DragView {
  source: DragSource;
  /** The pointer, relative to the grid. */
  x: number;
  y: number;
  target: 'group' | 'header' | 'outside' | null;
  valid: boolean;
  /** Where the dragged item would land, relative to the grid. */
  indicator: { left: number; top: number; height: number } | null;
}

function sameFields(a: readonly string[], b: readonly string[]): boolean {
  return a.length === b.length && a.every(field => b.includes(field));
}

const fieldsOf = (columns: readonly GridColumn<never>[]) => columns.map(column => column.field);

/**
 * A data grid in the manner of Kendo UI's: drag a column header into the group panel to group by it, click a header
 * to sort (Ctrl or Cmd adds to the sort), drag headers to reorder, drag their edge to resize, and use the column menu
 * to sort, group and show or hide columns.
 *
 * In 'client' mode the grid filters, sorts, groups and pages `data` itself. In 'server' mode `data` is one page that
 * the host loads on `stateChange`, sorted by `effectiveSort(state.group, state.sort)` so that groups stay together;
 * the grid only groups the page.
 *
 * ```html
 * <app-data-grid [data]="people" [columns]="[{ field: 'name' }, { field: 'team', title: 'Team' }]" [(group)]="group">
 *   <ng-template gridCell let-value let-row="row">{{ value }}</ng-template>
 * </app-data-grid>
 * ```
 */
@Component({
  selector: 'app-data-grid',
  imports: [
    NgTemplateOutlet,
    MatButtonModule,
    MatChipsModule,
    MatDividerModule,
    MatIconModule,
    MatMenuModule,
    MatPaginatorModule,
    MatTableModule,
  ],
  templateUrl: './data-grid.component.html',
  styleUrl: './data-grid.component.scss',
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class DataGridComponent<T = unknown> {
  private readonly host = inject<ElementRef<HTMLElement>>(ElementRef);
  private readonly destroyRef = inject(DestroyRef);

  readonly data = input<readonly T[]>([]);
  readonly columns = input<readonly GridColumn<T>[]>([]);
  readonly mode = input<'client' | 'server'>('client');
  /** Server mode: the number of rows in all pages. */
  readonly total = input(0);
  /** Server mode: the position of the first row of `data` among all rows; pageIndex × pageSize when null. */
  readonly skip = input<number | null>(null);
  readonly sortable = input<'none' | 'single' | 'multiple'>('multiple');
  readonly groupable = input(true);
  readonly reorderable = input(true);
  readonly resizable = input(true);
  readonly filterable = input(false);
  readonly columnMenu = input(true);
  readonly pageable = input(true);
  readonly pageSizes = input<number[]>([50, 100, 500, 1000]);
  readonly selectable = input<'none' | 'single' | 'multiple'>('none');
  readonly rowNumbers = input(false);
  /** Server mode: how long typing in a filter box pauses before `stateChange`. */
  readonly filterDelay = input(300);
  readonly emptyText = input('No records to display');

  readonly sort = model<SortDescriptor[]>([]);
  readonly group = model<GroupDescriptor[]>([]);
  readonly filter = model<Record<string, string>>({});
  readonly pageIndex = model(0);
  readonly pageSize = model(100);
  /** Indices in `data` of the selected rows. */
  readonly selection = model<ReadonlySet<number>>(new Set());
  readonly activeCell = model<CellRef | null>(null);

  /** The user changed the sort, the groups, a filter or the page. */
  readonly stateChange = output<GridState>();
  readonly cellClick = output<GridCellEvent<T>>();
  readonly cellDblClick = output<GridCellEvent<T>>();

  protected readonly cellTemplate = contentChild(GridCellDirective);
  protected readonly groupHeaderTemplate = contentChild(GridGroupHeaderDirective);

  protected readonly indentWidth = 32;
  protected readonly groupIds = ['__group'];
  protected readonly isGroupRow = (_: number, row: GridViewRow<T>) => row.kind === 'group';
  protected readonly trackRow = (_: number, row: GridViewRow<T>) => row.key;

  private readonly byField = computed(() => new Map(this.columns().map(column => [column.field, column])));

  /** Field order as the user arranged it; kept while the same columns come back (a reload). */
  protected readonly order = linkedSignal<readonly GridColumn<T>[], string[]>({
    source: this.columns,
    computation: (columns, previous) => (previous && sameFields(previous.value, fieldsOf(columns)) ? previous.value : fieldsOf(columns)),
  });
  protected readonly hidden = linkedSignal<readonly GridColumn<T>[], ReadonlySet<string>>({
    source: this.columns,
    computation: (columns, previous) =>
      previous && sameFields(fieldsOf(previous.source), fieldsOf(columns))
        ? previous.value
        : new Set(columns.filter(c => c.hidden).map(c => c.field)),
  });
  protected readonly widths = linkedSignal<readonly GridColumn<T>[], ReadonlyMap<string, number>>({
    source: this.columns,
    computation: (columns, previous) =>
      previous && sameFields(fieldsOf(previous.source), fieldsOf(columns))
        ? previous.value
        : new Map(columns.filter(c => c.width !== undefined).map(c => [c.field, c.width!])),
  });

  protected readonly orderedColumns = computed(() => this.order().map(field => this.byField().get(field)!));
  protected readonly visibleColumns = computed(() => this.orderedColumns().filter(column => !this.hidden().has(column.field)));
  protected readonly validGroup = computed(() => this.group().filter(g => this.byField().has(g.field)));
  private readonly validSort = computed(() => this.sort().filter(s => this.byField().has(s.field)));

  protected readonly groupLevels = computed(() => this.validGroup().map((_, level) => level));
  private readonly lead = computed(() => [...this.groupLevels().map(level => `__g${level}`), ...(this.rowNumbers() ? ['__rowno'] : [])]);
  protected readonly headerIds = computed(() => [...this.lead(), ...this.visibleColumns().map(c => `c-${c.field}`)]);
  protected readonly filterIds = computed(() => [
    ...this.groupLevels().map(level => `__fg${level}`),
    ...(this.rowNumbers() ? ['__frowno'] : []),
    ...this.visibleColumns().map(c => `f-${c.field}`),
  ]);

  private readonly processed = computed(() => {
    const items = this.data().map((row, index) => ({ row, index }));
    if (this.mode() === 'server') {
      return items;
    }
    const columns = this.columns();
    return sortItems(filterItems(items, columns, this.filter()), columns, effectiveSort(this.validGroup(), this.validSort()));
  });
  protected readonly length = computed(() => (this.mode() === 'server' ? this.total() : this.processed().length));
  private readonly firstPosition = computed(() => {
    if (this.mode() === 'server') {
      return this.skip() ?? this.pageIndex() * this.pageSize();
    }
    return this.pageable() ? this.pageIndex() * this.pageSize() : 0;
  });

  /** Keys of the collapsed groups; all open again when the grouping changes. */
  protected readonly collapsed = linkedSignal<readonly GroupDescriptor[], ReadonlySet<string>>({
    source: this.validGroup,
    computation: (group, previous) =>
      previous &&
      previous.source.length === group.length &&
      previous.source.every((g, i) => g.field === group[i].field && g.dir === group[i].dir)
        ? previous.value
        : new Set(),
  });

  protected readonly viewRows = computed(() => {
    let items = this.processed();
    if (this.mode() === 'client' && this.pageable()) {
      const first = this.pageIndex() * this.pageSize();
      items = items.slice(first, first + this.pageSize());
    }
    return buildView(items, this.columns(), this.validGroup(), this.collapsed(), this.firstPosition());
  });
  protected readonly dataRows = computed(() => this.viewRows().filter((row): row is GridDataRow<T> => row.kind === 'data'));

  protected readonly drag = signal<DragView | null>(null);
  private dropIndex = 0;
  /** A drag ends with a click on what was dragged, which must not sort. */
  private suppressClick = false;
  private anchorRow: number | null = null;
  private filterTimer: ReturnType<typeof setTimeout> | undefined;
  private stopTracking: (() => void) | null = null;

  constructor() {
    this.destroyRef.onDestroy(() => {
      clearTimeout(this.filterTimer);
      this.stopTracking?.();
    });
  }

  // ------------------------------------------------------------- columns ---

  protected titleOf(field: string): string {
    const column = this.byField().get(field);
    return column ? columnTitle(column) : field;
  }

  protected valueOf(column: GridColumn<T>, row: T): unknown {
    return columnValue(column, row);
  }

  protected textOf(column: GridColumn<T>, value: unknown): string {
    return columnText(column, value);
  }

  protected widthOf(field: string): number | null {
    return this.widths().get(field) ?? null;
  }

  protected canSort(column: GridColumn<T>): boolean {
    return this.sortable() !== 'none' && column.sortable !== false;
  }

  protected canGroup(column: GridColumn<T>): boolean {
    return this.groupable() && column.groupable !== false;
  }

  protected canReorder(column: GridColumn<T>): boolean {
    return this.reorderable() && column.reorderable !== false;
  }

  protected canResize(column: GridColumn<T>): boolean {
    return this.resizable() && column.resizable !== false;
  }

  protected canFilter(column: GridColumn<T>): boolean {
    return column.filterable !== false;
  }

  protected toggleHidden(field: string): void {
    const hidden = new Set(this.hidden());
    if (hidden.has(field)) {
      hidden.delete(field);
    } else if (this.visibleColumns().length > 1) {
      hidden.add(field);
    }
    this.hidden.set(hidden);
  }

  /** The columns as the host defined them: order, visibility and widths. */
  protected resetColumns(): void {
    const columns = this.columns();
    this.order.set(fieldsOf(columns));
    this.hidden.set(new Set(columns.filter(c => c.hidden).map(c => c.field)));
    this.widths.set(new Map(columns.filter(c => c.width !== undefined).map(c => [c.field, c.width!])));
  }

  /** Moves a visible column to before the visible column at `to`; hidden columns keep their places. */
  protected moveColumn(field: string, to: number): void {
    const visible = this.visibleColumns().map(c => c.field);
    const from = visible.indexOf(field);
    const moved = moveItem(visible, from, to);
    if (from < 0 || moved.every((f, i) => f === visible[i])) {
      return;
    }
    let next = 0;
    this.order.set(this.order().map(f => (this.hidden().has(f) ? f : moved[next++])));
  }

  protected autoSize(field: string): void {
    const widths = new Map(this.widths());
    widths.delete(field);
    this.widths.set(widths);
  }

  protected resizeStart(event: PointerEvent, field: string, header: HTMLElement): void {
    event.preventDefault();
    event.stopPropagation();
    const startX = event.clientX;
    const startWidth = header.getBoundingClientRect().width;
    this.track(
      move =>
        this.widths.set(new Map(this.widths()).set(field, Math.max(MIN_COLUMN_WIDTH_PX, Math.round(startWidth + move.clientX - startX)))),
      () => undefined,
    );
  }

  // ------------------------------------------------------------- sorting ---

  /** How a header shows its sort: the direction, and its place when several columns sort. A group sorts its column. */
  protected sortOf(field: string): { dir: SortDirection; order: number } | null {
    const grouped = this.validGroup().find(g => g.field === field);
    if (grouped) {
      return { dir: grouped.dir, order: 0 };
    }
    const sort = this.validSort();
    const at = sort.findIndex(s => s.field === field);
    return at < 0 ? null : { dir: sort[at].dir, order: sort.length > 1 ? at + 1 : 0 };
  }

  protected ariaSort(field: string): string | null {
    const sort = this.sortOf(field);
    return sort ? (sort.dir === 'asc' ? 'ascending' : 'descending') : null;
  }

  protected headerClick(event: Event, column: GridColumn<T>): void {
    if (event instanceof KeyboardEvent) {
      // Enter on the column menu button is the menu's.
      if (event.target !== event.currentTarget) {
        return;
      }
    } else if (this.suppressClick) {
      this.suppressClick = false;
      return;
    }
    if (!this.canSort(column)) {
      return;
    }
    if (this.validGroup().some(g => g.field === column.field)) {
      this.toggleGroupDir(column.field);
      return;
    }
    const keys = event as MouseEvent | KeyboardEvent;
    this.toggleSort(column.field, (keys.ctrlKey || keys.metaKey) && this.sortable() === 'multiple');
  }

  /** Ascending, descending, then unsorted. `add` keeps the other sorted columns. */
  protected toggleSort(field: string, add: boolean): void {
    const sort = this.validSort();
    const current = sort.find(s => s.field === field);
    const dir: SortDirection | null = !current ? 'asc' : current.dir === 'asc' ? 'desc' : null;
    if (!add) {
      this.setSort(dir ? [{ field, dir }] : []);
    } else if (!dir) {
      this.setSort(sort.filter(s => s.field !== field));
    } else {
      this.setSort(current ? sort.map(s => (s.field === field ? { field, dir } : s)) : [...sort, { field, dir }]);
    }
  }

  protected sortBy(field: string, dir: SortDirection): void {
    if (this.validGroup().some(g => g.field === field)) {
      this.setGroup(this.validGroup().map(g => (g.field === field ? { field, dir } : g)));
      return;
    }
    const sort = this.validSort();
    if (this.sortable() === 'multiple' && sort.some(s => s.field !== field)) {
      this.setSort(sort.some(s => s.field === field) ? sort.map(s => (s.field === field ? { field, dir } : s)) : [...sort, { field, dir }]);
    } else {
      this.setSort([{ field, dir }]);
    }
  }

  protected clearSort(field: string): void {
    this.setSort(this.validSort().filter(s => s.field !== field));
  }

  private setSort(sort: SortDescriptor[]): void {
    this.sort.set(sort);
    this.pageIndex.set(0);
    this.emitState();
  }

  // ------------------------------------------------------------ grouping ---

  protected isGrouped(field: string): boolean {
    return this.validGroup().some(g => g.field === field);
  }

  protected addGroup(field: string, at = this.validGroup().length): void {
    const column = this.byField().get(field);
    if (!column || !this.canGroup(column) || this.isGrouped(field)) {
      return;
    }
    const group = [...this.validGroup()];
    group.splice(at, 0, { field, dir: this.sortOf(field)?.dir ?? 'asc' });
    this.setGroup(group);
  }

  protected removeGroup(field: string): void {
    if (this.isGrouped(field)) {
      this.setGroup(this.validGroup().filter(g => g.field !== field));
    }
  }

  protected toggleGroupDir(field: string): void {
    this.setGroup(this.validGroup().map(g => (g.field === field ? { field, dir: g.dir === 'asc' ? 'desc' : 'asc' } : g)));
  }

  protected chipClick(event: Event, field: string): void {
    if (!(event instanceof KeyboardEvent) && this.suppressClick) {
      this.suppressClick = false;
      return;
    }
    this.toggleGroupDir(field);
  }

  protected toggleCollapsed(key: string): void {
    const collapsed = new Set(this.collapsed());
    if (!collapsed.delete(key)) {
      collapsed.add(key);
    }
    this.collapsed.set(collapsed);
  }

  private setGroup(group: GroupDescriptor[]): void {
    this.group.set(group);
    this.pageIndex.set(0);
    this.emitState();
  }

  // ------------------------------------------------- filtering and paging ---

  protected setFilter(field: string, text: string): void {
    const filter = { ...this.filter() };
    if (text.trim() === '') {
      delete filter[field];
    } else {
      filter[field] = text;
    }
    this.filter.set(filter);
    clearTimeout(this.filterTimer);
    if (this.mode() === 'client') {
      this.pageIndex.set(0);
      this.emitState();
      return;
    }
    this.filterTimer = setTimeout(() => {
      this.pageIndex.set(0);
      this.emitState();
    }, this.filterDelay());
  }

  protected onPage(event: PageEvent): void {
    this.pageIndex.set(event.pageIndex);
    this.pageSize.set(event.pageSize);
    this.emitState();
  }

  private emitState(): void {
    this.stateChange.emit({
      skip: this.pageIndex() * this.pageSize(),
      take: this.pageSize(),
      sort: this.validSort(),
      group: this.validGroup(),
      filter: this.filter(),
    });
  }

  // ------------------------------------------------ selection and cells ---

  protected isActive(index: number, field: string): boolean {
    const active = this.activeCell();
    return active?.row === index && active.field === field;
  }

  protected cellClicked(event: MouseEvent, row: GridDataRow<T>, column: GridColumn<T>): void {
    this.activeCell.set({ row: row.index, field: column.field });
    this.selectRow(row.index, event);
    this.cellClick.emit({ row: row.row, index: row.index, column, event });
  }

  protected cellDblClicked(event: MouseEvent, row: GridDataRow<T>, column: GridColumn<T>): void {
    this.cellDblClick.emit({ row: row.row, index: row.index, column, event });
  }

  /** Click selects one row, Ctrl or Cmd adds or removes one, Shift selects the rows shown between. */
  protected selectRow(index: number, event: MouseEvent): void {
    const mode = this.selectable();
    if (mode === 'none') {
      return;
    }
    const multi = mode === 'multiple';
    const selection = new Set(multi && (event.ctrlKey || event.metaKey) ? this.selection() : []);
    if (multi && event.shiftKey && this.anchorRow !== null) {
      const shown = this.dataRows().map(row => row.index);
      const a = shown.indexOf(this.anchorRow);
      const b = shown.indexOf(index);
      if (a >= 0 && b >= 0) {
        shown.slice(Math.min(a, b), Math.max(a, b) + 1).forEach(i => selection.add(i));
        this.selection.set(selection);
        return;
      }
    }
    if (multi && (event.ctrlKey || event.metaKey) && selection.has(index)) {
      selection.delete(index);
    } else {
      selection.add(index);
      this.anchorRow = index;
    }
    this.selection.set(selection);
  }

  // ------------------------------------------------------------ dragging ---

  protected headerPointerDown(event: PointerEvent, column: GridColumn<T>): void {
    this.suppressClick = false;
    if (event.button === 0 && (this.canReorder(column) || this.canGroup(column))) {
      this.startDrag(event, { kind: 'column', field: column.field, title: columnTitle(column) });
    }
  }

  protected chipPointerDown(event: PointerEvent, field: string): void {
    this.suppressClick = false;
    if (event.button === 0) {
      this.startDrag(event, { kind: 'group', field, title: this.titleOf(field) });
    }
  }

  protected hintIcon(view: DragView): string {
    if (!view.valid) {
      return 'block';
    }
    if (view.source.kind === 'group') {
      return view.target === 'group' ? 'swap_horiz' : 'remove';
    }
    return view.target === 'group' ? 'add' : 'swap_horiz';
  }

  private startDrag(down: PointerEvent, source: DragSource): void {
    let active = false;
    this.track(
      move => {
        if (!active && Math.hypot(move.clientX - down.clientX, move.clientY - down.clientY) < DRAG_THRESHOLD_PX) {
          return;
        }
        active = true;
        this.updateDrag(source, move.clientX, move.clientY);
      },
      () => {
        if (active) {
          this.suppressClick = true;
          this.finishDrag(source);
        }
        this.drag.set(null);
      },
    );
  }

  private updateDrag(source: DragSource, x: number, y: number): void {
    const root = this.host.nativeElement;
    const origin = root.getBoundingClientRect();
    const inside = (r: DOMRect) => x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    const before = (rects: DOMRect[]) => rects.filter(r => r.left + r.width / 2 < x).length;
    const column = this.byField().get(source.field);
    const panel = root.querySelector<HTMLElement>('.grid-group-panel');
    const header = root.querySelector<HTMLElement>('tr.grid-header-row');
    let view: DragView = { source, x: x - origin.left, y: y - origin.top, target: null, valid: false, indicator: null };
    let index = 0;
    if (panel && inside(panel.getBoundingClientRect())) {
      const box = panel.getBoundingClientRect();
      const chips = Array.from(panel.querySelectorAll<HTMLElement>('.grid-group-chip')).map(chip => chip.getBoundingClientRect());
      index = before(chips);
      const left = index < chips.length ? chips[index].left - 4 : chips.length > 0 ? chips[chips.length - 1].right + 4 : box.left + 8;
      view = {
        ...view,
        target: 'group',
        valid: source.kind === 'group' || (!!column && this.canGroup(column) && !this.isGrouped(source.field)),
        indicator: { left: left - origin.left, top: box.top + 8 - origin.top, height: box.height - 16 },
      };
    } else if (header && inside(header.getBoundingClientRect())) {
      view = { ...view, target: 'header', valid: source.kind === 'group' };
      if (source.kind === 'column' && column) {
        const box = header.getBoundingClientRect();
        const cells = Array.from(header.querySelectorAll<HTMLElement>('th[data-field]')).map(th => th.getBoundingClientRect());
        index = before(cells);
        const left = index < cells.length ? cells[index].left : cells[cells.length - 1].right;
        view = {
          ...view,
          valid: this.canReorder(column),
          indicator: { left: left - 1 - origin.left, top: box.top - origin.top, height: box.height },
        };
      }
    } else if (source.kind === 'group') {
      view = { ...view, target: 'outside', valid: true };
    }
    this.dropIndex = index;
    this.drag.set(view);
  }

  private finishDrag(source: DragSource): void {
    const view = this.drag();
    if (!view?.valid) {
      return;
    }
    if (source.kind === 'column') {
      if (view.target === 'group') {
        this.addGroup(source.field, this.dropIndex);
      } else if (view.target === 'header') {
        this.moveColumn(source.field, this.dropIndex);
      }
    } else if (view.target === 'group') {
      const group = this.validGroup();
      const moved = moveItem(
        group,
        group.findIndex(g => g.field === source.field),
        this.dropIndex,
      );
      if (moved.some((g, i) => g !== group[i])) {
        this.setGroup(moved);
      }
    } else {
      this.removeGroup(source.field);
    }
  }

  /** Follows the pointer until the button goes up. */
  private track(move: (event: PointerEvent) => void, end: () => void): void {
    this.stopTracking?.();
    const doc = this.host.nativeElement.ownerDocument;
    const onMove = (event: PointerEvent) => move(event);
    const onEnd = () => {
      stop();
      end();
    };
    const onCancel = () => {
      this.drag.set(null);
      onEnd();
    };
    const stop = () => {
      doc.removeEventListener('pointermove', onMove);
      doc.removeEventListener('pointerup', onEnd);
      doc.removeEventListener('pointercancel', onCancel);
      this.stopTracking = null;
    };
    doc.addEventListener('pointermove', onMove);
    doc.addEventListener('pointerup', onEnd);
    doc.addEventListener('pointercancel', onCancel);
    this.stopTracking = stop;
  }
}
