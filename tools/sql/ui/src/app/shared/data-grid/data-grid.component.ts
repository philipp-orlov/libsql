import { NgTemplateOutlet } from '@angular/common';
import {
  ChangeDetectionStrategy,
  Component,
  DestroyRef,
  ElementRef,
  afterNextRender,
  computed,
  contentChild,
  inject,
  input,
  linkedSignal,
  model,
  output,
  signal,
  viewChild,
} from '@angular/core';
import { MatIconModule } from '@angular/material/icon';
import { MatInputModule } from '@angular/material/input';
import { MatMenuModule } from '@angular/material/menu';
import { MatPaginatorModule, PageEvent } from '@angular/material/paginator';

import { GridCellDirective, GridGroupHeaderDirective } from './data-grid.directives';
import {
  CellRef,
  GridColumn,
  GridCellEvent,
  GridDataRow,
  GridGroupRow,
  GridState,
  GroupDescriptor,
  SortDescriptor,
  SortDirection,
} from './data-grid.models';
import { buildView, columnText, columnTitle, columnValue, effectiveSort, filterItems, moveItem, sortItems } from './data-grid.process';

const DRAG_THRESHOLD_PX = 5;
const MIN_COLUMN_WIDTH_PX = 48;
const MAX_FIT_WIDTH_PX = 600;
const INDENT_WIDTH_PX = 32;
const ROW_NUMBER_WIDTH_PX = 56;
/** Rows rendered above and below the visible ones, so a fast scroll shows no gap. */
const OVERSCAN_ROWS = 8;

/** A data row of the rendered window, with the values and texts of the visible columns. */
interface RenderedDataRow<T> extends GridDataRow<T> {
  cells: { value: unknown; text: string }[];
}

type RenderedRow<T> = GridGroupRow | RenderedDataRow<T>;

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
 * Only the rows in view are rendered (virtual scrolling): every row has the height `rowHeight`, and the grid needs a
 * bounded height to scroll in. Columns have fixed widths, so the layout does not move while rows scroll by.
 *
 * ```html
 * <app-data-grid [data]="people" [columns]="[{ field: 'name' }, { field: 'team', title: 'Team' }]" [(group)]="group">
 *   <ng-template gridCell let-value let-row="row">{{ value }}</ng-template>
 * </app-data-grid>
 * ```
 */
@Component({
  selector: 'app-data-grid',
  imports: [MatInputModule, NgTemplateOutlet, MatIconModule, MatMenuModule, MatPaginatorModule],
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
  readonly pageable = input(false);
  readonly pageSizes = input<number[]>([50, 100, 500, 1000]);
  readonly selectable = input<'none' | 'single' | 'multiple'>('none');
  readonly rowNumbers = input(false);
  /** Renders only the rows in view. Off renders every row (of the page). */
  readonly virtual = input(true);
  /** The height of every row in pixels, data and group rows alike. */
  readonly rowHeight = input(40);
  /** The width of a column that sets none and that the user has not resized. */
  readonly columnWidth = input(160);
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
  private readonly scroller = viewChild.required<ElementRef<HTMLElement>>('scroller');

  protected readonly indentWidth = INDENT_WIDTH_PX;
  protected readonly rowNumberWidth = ROW_NUMBER_WIDTH_PX;

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
  /** Widths the user set by dragging or fitting; the rest come from the columns. */
  protected readonly widths = linkedSignal<readonly GridColumn<T>[], ReadonlyMap<string, number>>({
    source: this.columns,
    computation: (columns, previous) => (previous && sameFields(fieldsOf(previous.source), fieldsOf(columns)) ? previous.value : new Map()),
  });

  protected readonly orderedColumns = computed(() => this.order().map(field => this.byField().get(field)!));
  protected readonly visibleColumns = computed(() => this.orderedColumns().filter(column => !this.hidden().has(column.field)));
  protected readonly validGroup = computed(() => this.group().filter(g => this.byField().has(g.field)));
  private readonly validSort = computed(() => this.sort().filter(s => this.byField().has(s.field)));

  protected readonly groupLevels = computed(() => this.validGroup().map((_, level) => level));
  /** Every column of the table: indents, row numbers, data columns and the filler that takes the rest. */
  protected readonly colCount = computed(() => this.groupLevels().length + (this.rowNumbers() ? 1 : 0) + this.visibleColumns().length + 1);
  protected readonly tableWidth = computed(
    () =>
      this.groupLevels().length * INDENT_WIDTH_PX +
      (this.rowNumbers() ? ROW_NUMBER_WIDTH_PX : 0) +
      this.visibleColumns().reduce((sum, column) => sum + this.widthOf(column), 0),
  );

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

  /** The scroll position and height of the rows' viewport; set by its listeners, outside change detection. */
  private readonly viewport = signal({ top: 0, height: 0 });
  /** The rows to render, [start, end) of viewRows. Equal ranges do not re-render. */
  private readonly range = computed(
    () => {
      const count = this.viewRows().length;
      if (!this.virtual()) {
        return { start: 0, end: count };
      }
      const height = this.rowHeight();
      const { top, height: shown } = this.viewport();
      const start = Math.min(count, Math.max(0, Math.floor(top / height) - OVERSCAN_ROWS));
      // Before the first measurement, a screenful is a fair guess.
      const end = Math.min(count, Math.ceil((top + (shown || 1000)) / height) + OVERSCAN_ROWS);
      return { start, end: Math.max(start, end) };
    },
    { equal: (a, b) => a.start === b.start && a.end === b.end },
  );
  protected readonly padTop = computed(() => this.range().start * this.rowHeight());
  protected readonly padBottom = computed(() => (this.viewRows().length - this.range().end) * this.rowHeight());
  protected readonly rendered = computed<RenderedRow<T>[]>(() => {
    const { start, end } = this.range();
    const columns = this.visibleColumns();
    return this.viewRows()
      .slice(start, end)
      .map(row =>
        row.kind === 'group'
          ? row
          : {
              ...row,
              cells: columns.map(column => {
                const value = columnValue(column, row.row);
                return { value, text: columnText(column, value) };
              }),
            },
      );
  });

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
    // Scrolling sets a signal and nothing else: a render happens only when the range of rows changes.
    afterNextRender(() => {
      const element = this.scroller().nativeElement;
      const measure = () => this.viewport.set({ top: element.scrollTop, height: element.clientHeight });
      element.addEventListener('scroll', measure, { passive: true });
      const observer = new ResizeObserver(measure);
      observer.observe(element);
      measure();
      this.destroyRef.onDestroy(() => {
        element.removeEventListener('scroll', measure);
        observer.disconnect();
      });
    });
  }

  private scrollToTop(): void {
    const element = this.scroller().nativeElement;
    element.scrollTop = 0;
    this.viewport.set({ top: 0, height: element.clientHeight });
  }

  // ------------------------------------------------------------- columns ---

  protected titleOf(field: string): string {
    const column = this.byField().get(field);
    return column ? columnTitle(column) : field;
  }

  protected widthOf(column: GridColumn<T>): number {
    return this.widths().get(column.field) ?? column.width ?? this.columnWidth();
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
    this.widths.set(new Map());
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

  /** Fits the column to its header and to the rows rendered now. */
  protected autoSize(field: string): void {
    const root = this.host.nativeElement;
    // Copies of the cell contents, laid out at their natural width.
    const probe = root.ownerDocument.createElement('div');
    probe.style.cssText =
      'position:absolute;left:-100000px;top:0;visibility:hidden;display:flex;align-items:center;gap:2px;white-space:nowrap;width:max-content';
    root.appendChild(probe);
    let widest = MIN_COLUMN_WIDTH_PX;
    for (const cell of Array.from(root.querySelectorAll<HTMLElement>(`[data-field="${CSS.escape(field)}"]`))) {
      const box = cell.querySelector('.grid-header-inner') ?? cell;
      const style = getComputedStyle(cell);
      probe.style.font = style.font;
      probe.replaceChildren(...Array.from(box.childNodes, node => node.cloneNode(true)));
      widest = Math.max(widest, Math.ceil(probe.offsetWidth + parseFloat(style.paddingLeft) + parseFloat(style.paddingRight) + 1));
    }
    probe.remove();
    this.widths.set(new Map(this.widths()).set(field, Math.min(MAX_FIT_WIDTH_PX, widest)));
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
      this.scrollToTop();
      this.emitState();
      return;
    }
    this.filterTimer = setTimeout(() => {
      this.pageIndex.set(0);
      this.scrollToTop();
      this.emitState();
    }, this.filterDelay());
  }

  protected onPage(event: PageEvent): void {
    this.pageIndex.set(event.pageIndex);
    this.pageSize.set(event.pageSize);
    this.scrollToTop();
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

  /** The data row and column of a click in the body; one listener serves every cell. */
  private hit(event: MouseEvent): { row: RenderedDataRow<T>; column: GridColumn<T> | null } | null {
    const target = event.target as HTMLElement;
    const tr = target.closest<HTMLElement>('tr[data-at]');
    const row = tr ? this.rendered()[Number(tr.dataset['at'])] : undefined;
    if (!row || row.kind !== 'data') {
      return null;
    }
    const field = target.closest<HTMLElement>('td[data-field]')?.dataset['field'];
    return { row, column: field === undefined ? null : (this.byField().get(field) ?? null) };
  }

  protected bodyClick(event: MouseEvent): void {
    const hit = this.hit(event);
    if (!hit) {
      return;
    }
    if (hit.column) {
      this.activeCell.set({ row: hit.row.index, field: hit.column.field });
    }
    this.selectRow(hit.row.index, event);
    if (hit.column) {
      this.cellClick.emit({ row: hit.row.row, index: hit.row.index, column: hit.column, event });
    }
  }

  protected bodyDblClick(event: MouseEvent): void {
    const hit = this.hit(event);
    if (hit?.column) {
      this.cellDblClick.emit({ row: hit.row.row, index: hit.row.index, column: hit.column, event });
    }
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
      const shown = this.viewRows().flatMap(row => (row.kind === 'data' ? [row.index] : []));
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
