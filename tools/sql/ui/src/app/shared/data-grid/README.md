# Data grid

`app-data-grid` is a standalone Angular component that shows rows in a table. It works like the Kendo UI grid and
looks like an Angular Material table. It uses only Angular Material and the Material 3 theme tokens
(`--mat-sys-*`), so it takes the colors, fonts and shapes of the application theme.

The user can:

- group rows: drag a column header into the group panel above the table. Drag more headers to nest the groups.
- reorder groups: drag a group chip to a new place in the panel.
- change the order of a group: click its chip, or click the header of the grouped column.
- ungroup: click the × of a chip, or drag the chip out of the panel.
- expand and collapse a group with the chevron in its header row.
- sort: click a header (ascending, descending, unsorted). Ctrl+click (Cmd+click on macOS) adds the column to the sort.
  Each sorted column shows its place in the sort when more than one column sorts.
- reorder columns: drag a header to a new place in the header row.
- resize a column: drag the right edge of its header. Double-click the edge to fit the column to its content again.
- use the column menu (the ⋮ button of a header): sort, clear the sort, group, ungroup, fit to content, show or hide
  columns, and reset the columns.
- filter: type in the box under a header (when `filterable` is on).
- page through the rows with the Material paginator.
- select rows: click, Ctrl+click and Shift+click (when `selectable` is on).

## Files

| File | Contents |
| --- | --- |
| `data-grid.component.ts/.html/.scss` | The component. |
| `data-grid.directives.ts` | `gridCell` and `gridGroupHeader`, the template directives. |
| `data-grid.models.ts` | The public types: columns, descriptors, state, events. |
| `data-grid.process.ts` | Filtering, sorting and grouping as pure functions, free of Angular. |
| `index.ts` | The public API. Import from the folder, not from the files. |

```ts
import { DataGridComponent, GridCellDirective, GridColumn } from '../shared/data-grid';
```

## Quick start

```ts
import { Component, signal } from '@angular/core';

import { DataGridComponent, GridColumn, GroupDescriptor } from '../shared/data-grid';

interface Person {
  name: string;
  team: string;
  score: number;
}

@Component({
  selector: 'app-people',
  imports: [DataGridComponent],
  template: `<app-data-grid class="grid" [data]="people" [columns]="columns" [(group)]="group" />`,
  styles: `
    .grid {
      height: 480px;
    }
  `,
})
export class PeopleComponent {
  readonly people: Person[] = [
    { name: 'Ada', team: 'red', score: 3 },
    { name: 'Bob', team: 'blue', score: 1 },
    { name: 'Cyd', team: 'red', score: 1 },
  ];
  readonly columns: GridColumn<Person>[] = [
    { field: 'name', title: 'Name' },
    { field: 'team', title: 'Team' },
    { field: 'score', title: 'Score', width: 96 },
  ];
  readonly group = signal<GroupDescriptor[]>([{ field: 'team', dir: 'asc' }]);
}
```

The grid fills its host element and scrolls inside it. Give the host a height, or make it a flex child that can
shrink (`flex: 1; min-height: 0`).

## Columns

A column is a `GridColumn<T>`:

| Property | Type | Default | Meaning |
| --- | --- | --- | --- |
| `field` | `string` | required | Identifies the column. Must be unique within the grid. |
| `title` | `string` | `field` | The header text. |
| `value` | `(row: T) => unknown` | `row[field]` | Reads the value from a row. |
| `text` | `(value: unknown) => string` | `String(value)`, `''` for null | The text that shows, groups and filters a value. |
| `compare` | `(a, b) => number` | `defaultCompare` | Orders two values for sorting and grouping. |
| `width` | `number` | content | Starting width in pixels. |
| `sortable` | `boolean` | `true` | `false` keeps the column out of sorting. |
| `groupable` | `boolean` | `true` | `false` keeps the column out of grouping. |
| `reorderable` | `boolean` | `true` | `false` keeps the column in its place. |
| `resizable` | `boolean` | `true` | `false` removes the resize edge. |
| `filterable` | `boolean` | `true` | `false` removes the filter box of the column. |
| `hidden` | `boolean` | `false` | The column starts hidden; the column menu shows it again. |

`defaultCompare` puts empty values first, then numbers, booleans and text, and then everything else by its text.

Give the grid the same `columns` array while the columns do not change. When the array changes, the grid keeps the
column order, the hidden columns and the widths that the user set, as long as the new array has the same fields. A
different set of fields resets them.

## Inputs

| Input | Type | Default | Meaning |
| --- | --- | --- | --- |
| `data` | `readonly T[]` | `[]` | The rows. In server mode, the rows of the current page. |
| `columns` | `readonly GridColumn<T>[]` | `[]` | The columns, in their starting order. |
| `mode` | `'client' \| 'server'` | `'client'` | Who filters, sorts and pages the rows (see [Modes](#modes)). |
| `total` | `number` | `0` | Server mode: the number of rows on all pages. |
| `skip` | `number \| null` | `null` | Server mode: the position of the first row of `data`. `pageIndex × pageSize` when null. |
| `sortable` | `'none' \| 'single' \| 'multiple'` | `'multiple'` | `'multiple'` lets Ctrl+click add columns to the sort. |
| `groupable` | `boolean` | `true` | Shows the group panel. |
| `reorderable` | `boolean` | `true` | Lets the user drag headers to new places. |
| `resizable` | `boolean` | `true` | Lets the user drag the edge of a header. |
| `filterable` | `boolean` | `false` | Shows a row of filter boxes under the headers. |
| `columnMenu` | `boolean` | `true` | Shows the ⋮ menu in each header. |
| `pageable` | `boolean` | `true` | Shows the paginator. Client mode without paging shows all rows. |
| `pageSizes` | `number[]` | `[50, 100, 500, 1000]` | The page sizes the paginator offers. |
| `selectable` | `'none' \| 'single' \| 'multiple'` | `'none'` | How clicks select rows. |
| `rowNumbers` | `boolean` | `false` | Shows a column of row numbers. A click on a number selects the row. |
| `filterDelay` | `number` | `300` | Server mode: milliseconds of no typing before a filter change is reported. |
| `emptyText` | `string` | `'No records to display'` | The text of an empty grid. |

## Two-way state

Each of these is a `model()`. Bind it with `[(name)]` to read and set it, or with `[name]` to set only the start
value.

| Model | Type | Meaning |
| --- | --- | --- |
| `sort` | `SortDescriptor[]` | The sorted columns, first key first: `{ field, dir: 'asc' \| 'desc' }`. |
| `group` | `GroupDescriptor[]` | The groups, outermost first: `{ field, dir }`. |
| `filter` | `Record<string, string>` | Per field, the text a value must contain. Case is ignored in client mode. |
| `pageIndex` | `number` | The current page, from 0. |
| `pageSize` | `number` | Rows per page. |
| `selection` | `ReadonlySet<number>` | The indices in `data` of the selected rows. |
| `activeCell` | `CellRef \| null` | The cell clicked last: `{ row: index in data, field }`. Shows an outline. |

A change of the sort, the groups or a filter goes back to the first page.

## Outputs

| Output | Payload | When |
| --- | --- | --- |
| `stateChange` | `GridState` | The user changed the sort, the groups, a filter or the page. |
| `cellClick` | `GridCellEvent<T>` | A data cell was clicked. |
| `cellDblClick` | `GridCellEvent<T>` | A data cell was double-clicked. |

`GridState` is `{ skip, take, sort, group, filter }`. `GridCellEvent<T>` is `{ row, index, column, event }`, where
`index` is the index of the row in `data`.

## Templates

### Cells

Put an `<ng-template gridCell>` inside the grid to render the data cells. The context gives:

| Variable | Value |
| --- | --- |
| `let-value` | The value of the column for the row (`$implicit`). |
| `let-row="row"` | The row. |
| `let-index="index"` | The index of the row in `data`. |
| `let-column="column"` | The `GridColumn`. |
| `let-text="text"` | The value as the column shows it as text. |

```html
<app-data-grid [data]="orders" [columns]="columns">
  <ng-template gridCell let-value let-column="column" let-text="text">
    @switch (column.field) {
      @case ('total') {
        <span class="money">{{ $any(value) | currency: 'EUR' }}</span>
      }
      @case ('status') {
        <mat-icon [class.late]="value === 'late'">{{ value === 'late' ? 'schedule' : 'check' }}</mat-icon>
      }
      @default {
        {{ text }}
      }
    }
  </ng-template>
</app-data-grid>
```

The template belongs to the host component, so the styles of the host apply to it.

### Group headers

Put an `<ng-template gridGroupHeader let-group>` inside the grid to render what follows the chevron of a group
header row. `group` is a `GridGroupRow`: `level`, `field`, `title`, `value`, `text`, `count` and `collapsed`.

```html
<app-data-grid [data]="people" [columns]="columns" [group]="[{ field: 'team', dir: 'asc' }]">
  <ng-template gridGroupHeader let-group>
    <strong>{{ group.text || '(none)' }}</strong>
    <span class="muted">{{ group.count }} {{ group.count === 1 ? 'person' : 'people' }}</span>
  </ng-template>
</app-data-grid>
```

## Modes

### Client mode

The default. The grid filters, sorts, groups and pages `data` in the browser. Use it when all rows are in memory.
Rows are sorted by the group fields first and then by `sort`, so each group appears once.

### Server mode

Set `mode="server"` when the rows come from a server one page at a time. Then:

1. The grid does not filter, sort or page. It shows `data` as it comes.
2. The grid reports each user change in `stateChange`.
3. The host loads the page that the state asks for and sets `data`, `total` and, if it wants, `skip`.
4. The grid groups the rows of the page: each run of equal values becomes a group.

Groups stay together only if the server sorts by the group fields first. `effectiveSort(group, sort)` gives that
order: the group descriptors, then the sort descriptors of the other columns.

```ts
import { Component, inject, signal } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { firstValueFrom } from 'rxjs';

import { DataGridComponent, GridColumn, GridState, effectiveSort } from '../shared/data-grid';

interface Order {
  id: number;
  customer: string;
  total: number;
}

interface Page<T> {
  rows: T[];
  total: number;
}

@Component({
  selector: 'app-orders',
  imports: [DataGridComponent],
  template: `
    <app-data-grid
      class="grid"
      mode="server"
      [data]="rows()"
      [total]="total()"
      [columns]="columns"
      [filterable]="true"
      (stateChange)="load($event)"
    />
  `,
})
export class OrdersComponent {
  private readonly http = inject(HttpClient);

  readonly columns: GridColumn<Order>[] = [
    { field: 'id', title: 'Order', groupable: false },
    { field: 'customer', title: 'Customer' },
    { field: 'total', title: 'Total' },
  ];
  readonly rows = signal<Order[]>([]);
  readonly total = signal(0);

  constructor() {
    void this.load({ skip: 0, take: 100, sort: [], group: [], filter: {} });
  }

  async load(state: GridState): Promise<void> {
    const page = await firstValueFrom(
      this.http.post<Page<Order>>('/api/orders', {
        offset: state.skip,
        limit: state.take,
        orderBy: effectiveSort(state.group, state.sort),
        filter: state.filter,
      }),
    );
    this.rows.set(page.rows);
    this.total.set(page.total);
  }
}
```

A late answer can overwrite a newer one. If requests can overlap, count them and keep only the answer to the last
one (see `BrowseComponent.load`).

In server mode the counts in the group headers count the rows on the current page only, and a group that spans two
pages shows a header on each page.

## More examples

### Rows that are arrays

Rows do not have to be objects. Give each column a `value` function. The SQL tool does this for result rows
(`cellColumn` in `utils/cell.ts`):

```ts
const names = ['id', 'name', 'id'];
const columns: GridColumn<unknown[]>[] = names.map((name, i) => ({
  // Two result columns can share a name, so the field is the position.
  field: `c${i}`,
  title: name,
  value: row => row[i],
}));
```

### Custom order and text

```ts
const priorities = ['low', 'normal', 'high', 'urgent'];

const columns: GridColumn<Ticket>[] = [
  {
    field: 'priority',
    title: 'Priority',
    // Sorts and groups by rank, not by spelling.
    compare: (a, b) => priorities.indexOf(String(a)) - priorities.indexOf(String(b)),
  },
  {
    field: 'due',
    title: 'Due',
    value: ticket => ticket.due?.getTime() ?? null,
    text: value => (value === null ? '' : new Date(value as number).toLocaleDateString()),
  },
];
```

The `text` function also decides the group header text and what the filter box matches.

### Starting state

Set the models once to start grouped, sorted or filtered:

```html
<app-data-grid
  [data]="tickets"
  [columns]="columns"
  [group]="[{ field: 'priority', dir: 'desc' }]"
  [sort]="[{ field: 'due', dir: 'asc' }]"
  [filterable]="true"
  [filter]="{ assignee: 'ada' }"
  [pageSize]="50"
/>
```

### Selection and editing

Bind `selection` and `activeCell` to act on the rows the user picks. Use `cellDblClick` and a cell template to edit a
value in place. This is how Browse Data edits a cell:

```ts
import { Component, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';

import { CellRef, DataGridComponent, GridCellDirective, GridColumn } from '../shared/data-grid';

@Component({
  selector: 'app-people-editor',
  imports: [DataGridComponent, GridCellDirective, MatButtonModule],
  template: `
    <button mat-button [disabled]="selected().size === 0" (click)="remove()">Delete</button>
    <app-data-grid
      [data]="people()"
      [columns]="columns"
      selectable="multiple"
      [rowNumbers]="true"
      [(selection)]="selected"
      (cellDblClick)="editing.set({ row: $event.index, field: $event.column.field })"
    >
      <ng-template gridCell let-index="index" let-column="column" let-text="text">
        @if (editing()?.row === index && editing()?.field === column.field) {
          <input #box [value]="text" (keydown.enter)="save(index, column.field, box.value)" (keydown.escape)="editing.set(null)" />
        } @else {
          {{ text }}
        }
      </ng-template>
    </app-data-grid>
  `,
})
export class PeopleEditorComponent {
  readonly people = signal<Person[]>([]);
  readonly columns: GridColumn<Person>[] = [{ field: 'name' }, { field: 'team' }];
  readonly selected = signal<ReadonlySet<number>>(new Set());
  readonly editing = signal<CellRef | null>(null);

  remove(): void {
    const gone = this.selected();
    this.people.update(people => people.filter((_, i) => !gone.has(i)));
    this.selected.set(new Set());
  }

  save(index: number, field: string, text: string): void {
    this.people.update(people => people.map((p, i) => (i === index ? { ...p, [field]: text } : p)));
    this.editing.set(null);
  }
}
```

Selection holds indices in `data`. Clear it when `data` changes to other rows.

### A fixed, plain table

Turn off what the user must not change:

```html
<app-data-grid
  [data]="rows"
  [columns]="columns"
  sortable="none"
  [groupable]="false"
  [reorderable]="false"
  [resizable]="false"
  [columnMenu]="false"
  [pageable]="false"
/>
```

### Using the processing functions alone

`data-grid.process.ts` does not depend on Angular. `effectiveSort` and `defaultCompare` are part of the public API,
for example to sort an export the way the grid shows it:

```ts
import { defaultCompare, effectiveSort } from '../shared/data-grid';

const order = effectiveSort(group, sort);
const sorted = [...rows].sort((a, b) => {
  for (const { field, dir } of order) {
    const c = defaultCompare(a[field], b[field]);
    if (c !== 0) {
      return dir === 'asc' ? c : -c;
    }
  }
  return 0;
});
```

## Styling

The grid has no styles of its own for color or type: it uses the Material 3 tokens of the theme. To change the
density of the rows, set the Material table tokens on the grid element:

```scss
app-data-grid.compact {
  --mat-table-row-item-container-height: 36px;
  --mat-table-header-container-height: 40px;
}
```

These classes are stable and can be used as hooks in host styles and in tests:

| Class | Element |
| --- | --- |
| `grid-group-panel`, `grid-group-chip`, `grid-group-hint` | The group panel, its chips and its empty-state text. |
| `grid-header-row`, `grid-header`, `sortable`, `title`, `sort`, `sort-order` | The header row, a header cell and its parts. |
| `grid-menu-button`, `grid-resize` | The column menu button and the resize edge of a header. |
| `grid-filter-row`, `filter` | The filter row and its boxes. |
| `grid-row`, `selected`, `active`, `rowno` | A data row, a selected row, the active cell, a row number cell. |
| `grid-group-row`, `grid-group-toggle`, `grid-group-title`, `grid-group-value`, `grid-group-count` | A group header row and its parts. |
| `grid-pager` | The paginator. |

The drag hint and the drop indicator are placed inside the grid element. Do not give the grid element
`overflow: hidden`, or the hint is cut off near its edges.

## Accessibility

- Sortable headers can take the focus. Enter sorts, and `aria-sort` reports the order.
- Group chips can take the focus. Enter reverses the order of the group.
- The chevron of a group header is a button with `aria-expanded`.
- The column menu is a Material menu, keyboard accessible. Its column items are `menuitemcheckbox` items.
- Dragging needs a pointer. The column menu gives the same grouping and showing or hiding of columns without one.

## Limitations

- Each `field` must be unique within a grid.
- Client-mode filters test whether the text of a value contains the filter text. For other filters, use server mode
  or filter `data` before the grid gets it.
- The grid renders every row of a page. Keep pages to a few thousand rows; there is no virtual scrolling.
- Groups have no footers or aggregates other than the row count.
- Collapsed groups open again when the grouping changes or another page is shown.

## Tests

`data-grid.process.spec.ts` covers the processing functions. `data-grid.component.spec.ts` drives a host component
in a real browser, with pointer events for the drag operations. Run them with the other tests of the UI:

```sh
CHROME_BIN=/path/to/chrome npm test
```
