// The public types of the data grid: columns, the sort, group and filter descriptors, and the state the grid reports.

export type SortDirection = 'asc' | 'desc';

export interface SortDescriptor {
  field: string;
  dir: SortDirection;
}

/** Rows are grouped by `field`; the groups are ordered by `dir`. */
export interface GroupDescriptor {
  field: string;
  dir: SortDirection;
}

export interface GridColumn<T = unknown> {
  /** Identifies the column; unique within a grid. */
  field: string;
  /** The header text; the field when not given. */
  title?: string;
  /** Reads the column's value from a row; `row[field]` when not given. */
  value?: (row: T) => unknown;
  /** The text that shows, groups and filters a value; `String(value)` and '' for null when not given. */
  text?: (value: unknown) => string;
  /** Orders two values for sorting and grouping; numbers, then text, when not given. */
  compare?: (a: unknown, b: unknown) => number;
  /** Starting width in pixels; the content decides when not given. */
  width?: number;
  sortable?: boolean;
  groupable?: boolean;
  reorderable?: boolean;
  resizable?: boolean;
  filterable?: boolean;
  /** Starts out hidden; the column menu shows it again. */
  hidden?: boolean;
}

/** What the grid shows. A host that processes the data itself (mode 'server') loads rows for it. */
export interface GridState {
  skip: number;
  take: number;
  sort: SortDescriptor[];
  group: GroupDescriptor[];
  /** Per field, the text a value must contain. */
  filter: Record<string, string>;
}

/** A cell, by the index of its row in the grid's data and the field of its column. */
export interface CellRef {
  row: number;
  field: string;
}

export interface GridCellEvent<T = unknown> {
  row: T;
  /** Index of the row in the grid's data. */
  index: number;
  column: GridColumn<T>;
  event: MouseEvent;
}

/** A row the grid renders: a group header or a data row. */
export type GridViewRow<T = unknown> = GridGroupRow | GridDataRow<T>;

export interface GridGroupRow {
  kind: 'group';
  /** Unique among the groups: the path of values from the outermost group. */
  key: string;
  /** 0 for the outermost group. */
  level: number;
  field: string;
  title: string;
  value: unknown;
  text: string;
  /** Data rows in the group, nested groups included. */
  count: number;
  collapsed: boolean;
}

export interface GridDataRow<T = unknown> {
  kind: 'data';
  key: string;
  row: T;
  /** Index of the row in the grid's data. */
  index: number;
  /** 0-based position among the processed rows (after filtering and sorting, before paging). */
  position: number;
}
