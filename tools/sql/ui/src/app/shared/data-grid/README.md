# Data grid

`app-data-grid` is a standalone Angular component that shows rows in a table. It works like the Kendo UI grid and
looks like an Angular Material table. It uses the Material 3 theme tokens (`--mat-sys-*`), so it takes the colors,
fonts and shapes of the application theme.

The grid is light:

- It renders only the rows in view (virtual scrolling), so 100,000 rows cost about as much to show as 20.
- It is a plain `<table>` with fixed column widths. It does not use `mat-table` or `mat-chip`, and the browser lays
  out no cell to find the widths of the columns.
- One click listener and one double-click listener serve all cells.
- Scrolling sets a signal and nothing else. Angular renders again only when the range of rows in view changes.
- From Angular Material it takes only `mat-icon`, the column menu (`mat-menu`, created when it opens) and, when
  paging is on, `mat-paginator`.

The user can:

- group rows: drag a column header into the group panel above the table. Drag more headers to nest the groups.
- reorder groups: drag a group chip to a new place in the panel.
- change the order of a group: click its chip, or click the header of the grouped column.
- ungroup: click the × of a chip, or drag the chip out of the panel.
- expand and collapse a group with the chevron in its header row.
- sort: click a header (ascending, descending, unsorted). Ctrl+click (Cmd+click on macOS) adds the column to the sort.
  Each sorted column shows its place in the sort when more than one column sorts.
- reorder columns: drag a header to a new place in the header row.
- resize a column: drag the right edge of its header. Double-click the edge to fit the column to its content.
- use the column menu (the ⋮ button of a header): sort, clear the sort, group, ungroup, fit to content, show or hide
  columns, and reset the columns.
- filter: type in the box under a header (when `filterable` is on).
- scroll through all rows, or page through them with the Material paginator (when `pageable` is on).
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
shrink (`flex: 1; min-height: 0`). Without a bounded height there is nothing to scroll, and every row is rendered.

## Columns

A column is a `GridColumn<T>`:

| Property | Type | Default | Meaning |
| --- | --- | --- | --- |
| `field` | `string` | required | Identifies the column. Must be unique within the grid. |
| `title` | `string` | `field` | The header text. |
| `value` | `(row: T) => unknown` | `row[field]` | Reads the value from a row. |
| `text` | `(value: unknown) => string` | `String(value)`, `''` for null | The text that shows, groups and filters a value. |
| `compare` | `(a, b) => number` | `defaultCompare` | Orders two values for sorting and grouping. |
| `width` | `number` | `columnWidth` | Starting width in pixels. |
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
| `pageable` | `boolean` | `false` | Shows the paginator. Without it, client mode scrolls through all rows. |
| `pageSizes` | `number[]` | `[50, 100, 500, 1000]` | The page sizes the paginator offers. |
| `selectable` | `'none' \| 'single' \| 'multiple'` | `'none'` | How clicks select rows. |
| `rowNumbers` | `boolean` | `false` | Shows a column of row numbers. A click on a number selects the row. |
| `virtual` | `boolean` | `true` | Renders only the rows in view. `false` renders every row (of the page). |
| `rowHeight` | `number` | `40` | The height of every row in pixels, data rows and group rows alike. |
| `columnWidth` | `number` | `160` | The width of a column that sets no `width` and that the user has not resized. |
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

## Virtual scrolling

The grid renders only the rows in view and a few rows above and below them (`OVERSCAN_ROWS`). Two empty spacer rows
take the height of the rows that are not rendered, so the scroll bar shows the full length. The rest of the work is
limited to the rows in view too: a value is read and its text is made only for a rendered cell.

To make this work:

- Every row has the same height, `rowHeight`. A group header row has it too. Content that is taller is cut off, so
  set `rowHeight` for the tallest content, for example 48 for 32-pixel images.
- The grid needs a bounded height (see [Quick start](#quick-start)).
- The columns have fixed widths: `width` of the column, else `columnWidth`. The user can resize them and fit them to
  the content in view.

Virtual scrolling and paging work together: with `pageable` on, the grid scrolls within the page. In client mode a
paginator is seldom useful; in server mode it chooses which rows the host loads.

Turn virtual scrolling off (`[virtual]="false"`) for content of different heights. Then every row is rendered, so
keep the number of rows (or the page size) small.

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
      [pageable]="true"
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

### Images and icons in cells

A cell template can hold any markup. This grid shows an avatar image next to each name, a status icon, and a
rating as a row of star icons. The rows are 48 pixels high to fit the 32-pixel images.

```ts
import { Component } from '@angular/core';
import { MatIconModule } from '@angular/material/icon';

import { DataGridComponent, GridCellDirective, GridColumn } from '../shared/data-grid';

interface Contact {
  id: number;
  name: string;
  photo: string;
  status: 'online' | 'away' | 'offline';
  rating: number;
}

const STATUS_ICON = { online: 'check_circle', away: 'schedule', offline: 'do_not_disturb_on' };
const STATUS_RANK = { online: 0, away: 1, offline: 2 };

@Component({
  selector: 'app-contacts',
  imports: [DataGridComponent, GridCellDirective, MatIconModule],
  template: `
    <app-data-grid class="grid" [data]="contacts" [columns]="columns" [rowHeight]="48" [filterable]="true">
      <ng-template gridCell let-value let-row="row" let-column="column" let-text="text">
        @switch (column.field) {
          @case ('name') {
            <span class="person">
              <img class="avatar" [src]="row.photo" alt="" width="32" height="32" decoding="async" />
              <span class="value">{{ text }}</span>
            </span>
          }
          @case ('status') {
            <span class="status" [class]="value">
              <mat-icon>{{ statusIcon[$any(value)] }}</mat-icon>
              {{ text }}
            </span>
          }
          @case ('rating') {
            <span class="stars" [attr.aria-label]="value + ' of 5'">
              @for (star of [1, 2, 3, 4, 5]; track star) {
                <mat-icon>{{ star <= $any(value) ? 'star' : 'star_border' }}</mat-icon>
              }
            </span>
          }
          @default {
            {{ text }}
          }
        }
      </ng-template>
    </app-data-grid>
  `,
  styles: `
    .grid {
      height: 560px;
    }
    .person,
    .status,
    .stars {
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .avatar {
      flex: none;
      border-radius: 50%;
      object-fit: cover;
    }
    .status mat-icon,
    .stars mat-icon {
      width: 18px;
      height: 18px;
      font-size: 18px;
    }
    .stars {
      gap: 0;
      color: var(--mat-sys-tertiary);
    }
    .online {
      color: var(--mat-sys-primary);
    }
    .offline {
      color: var(--mat-sys-outline);
    }
  `,
})
export class ContactsComponent {
  protected readonly statusIcon = STATUS_ICON;

  readonly contacts: Contact[] = Array.from({ length: 50000 }, (_, i) => ({
    id: i + 1,
    name: `Contact ${i + 1}`,
    photo: `/avatars/${i % 100}.webp`,
    status: (['online', 'away', 'offline'] as const)[i % 3],
    rating: (i % 5) + 1,
  }));

  readonly columns: GridColumn<Contact>[] = [
    { field: 'id', title: 'ID', width: 88 },
    { field: 'name', title: 'Name', width: 240 },
    // Groups and sorts by state, not by spelling.
    { field: 'status', title: 'Status', width: 140, compare: (a, b) => STATUS_RANK[a as Contact['status']] - STATUS_RANK[b as Contact['status']] },
    { field: 'rating', title: 'Rating', width: 140, filterable: false },
  ];
}
```

Notes:

- Only the rows in view have `<img>` elements, so of 50,000 photos the browser loads only those that come into view.
  `loading="lazy"` adds nothing here.
- Give an image its `width` and `height`, so its row does not change size while it loads.
- The template belongs to the host, so `.avatar`, `.status` and `.stars` are styled by the host's styles.
- The `text` of a column still drives grouping and filtering: the status column groups as "online", "away" and
  "offline", and the filter box finds "away".
- An icon-only cell reads as nothing to a screen reader. Give it a label (`aria-label`) or visible text.

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
  [pageable]="true"
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

Turn off what the user must not change. For a short table that takes the height of its rows, turn virtual scrolling
off as well:

```html
<app-data-grid
  [data]="rows"
  [columns]="columns"
  sortable="none"
  [groupable]="false"
  [reorderable]="false"
  [resizable]="false"
  [columnMenu]="false"
  [virtual]="false"
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

The grid has no styles of its own for color or type: it uses the Material 3 tokens of the theme. Set the density of
the rows with `rowHeight` (the header row is 48 pixels high).

These classes are stable and can be used as hooks in host styles and in tests:

| Class | Element |
| --- | --- |
| `grid-group-panel`, `grid-group-chip`, `grid-chip-remove`, `grid-group-hint` | The group panel, its chips, the × of a chip, the empty-state text. |
| `grid-scroll` | The element that scrolls the rows. |
| `grid-header-row`, `grid-header`, `sortable`, `title`, `sort`, `sort-order` | The header row, a header cell and its parts. |
| `grid-menu-button`, `grid-resize` | The column menu button and the resize edge of a header. |
| `grid-filter-row`, `filter` | The filter row and its boxes. |
| `grid-row`, `grid-cell`, `selected`, `active`, `rowno` | A data row, a data cell, a selected row, the active cell, a row number cell. |
| `grid-group-row`, `grid-group-toggle`, `grid-group-title`, `grid-group-value`, `grid-group-count` | A group header row and its parts. |
| `grid-filler` | The empty last column that takes the width the columns leave. |
| `grid-spacer` | The rows that stand in for the rows not rendered. |
| `grid-pager` | The paginator. |

A data cell has `data-field` set to the field of its column.

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
- Every row has one height (see [Virtual scrolling](#virtual-scrolling)).
- Browsers limit the height of an element (about 33 million pixels in Chrome), so one scroll can hold about 800,000
  rows of 40 pixels. Page anything larger.
- Client mode still filters, sorts and groups every row in memory; only the rendering is limited to the rows in view.
- Fit to content measures the header and the rows rendered at that moment, not every row.
- Groups have no footers or aggregates other than the row count.
- Collapsed groups open again when the grouping changes or another page is shown.

## Tests

`data-grid.process.spec.ts` covers the processing functions. `data-grid.component.spec.ts` drives a host component
in a real browser, with pointer events for the drag operations, and scrolls a grid of 10,000 rows with images and
icons in its cells. Run them with the other tests of the UI:

```sh
CHROME_BIN=/path/to/chrome npm test
```
