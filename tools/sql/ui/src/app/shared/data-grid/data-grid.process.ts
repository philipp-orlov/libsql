// Data operations of the grid, free of Angular: filtering, sorting and grouping rows into what the grid renders.

import { GridColumn, GridDataRow, GridGroupRow, GridViewRow, GroupDescriptor, SortDescriptor } from './data-grid.models';

/** A row with its index in the grid's data. */
export interface GridItem<T> {
  row: T;
  index: number;
}

function rank(value: unknown): number {
  if (value === null || value === undefined) {
    return 0;
  }
  if (typeof value === 'number' || typeof value === 'bigint') {
    return 1;
  }
  if (typeof value === 'boolean') {
    return 2;
  }
  if (typeof value === 'string') {
    return 3;
  }
  return 4;
}

/** Empty values first, then numbers, booleans, text, and anything else by its text. */
export function defaultCompare(a: unknown, b: unknown): number {
  const ra = rank(a);
  const rb = rank(b);
  if (ra !== rb || ra === 0) {
    return ra - rb;
  }
  if (ra === 4) {
    a = String(a);
    b = String(b);
  }
  return (a as number | string) < (b as number | string) ? -1 : (a as number | string) > (b as number | string) ? 1 : 0;
}

export function columnTitle(column: GridColumn<never>): string {
  return column.title ?? column.field;
}

export function columnValue<T>(column: GridColumn<T>, row: T): unknown {
  return column.value ? column.value(row) : (row as Record<string, unknown>)[column.field];
}

export function columnText(column: GridColumn<never>, value: unknown): string {
  if (column.text) {
    return column.text(value);
  }
  return value === null || value === undefined ? '' : String(value);
}

function compareBy(column: GridColumn<never>): (a: unknown, b: unknown) => number {
  return column.compare ?? defaultCompare;
}

/** The sort that keeps groups together: the group fields first, then the rest of `sort`. */
export function effectiveSort(group: readonly GroupDescriptor[], sort: readonly SortDescriptor[]): SortDescriptor[] {
  const grouped = new Set(group.map(g => g.field));
  return [...group.map(g => ({ field: g.field, dir: g.dir })), ...sort.filter(s => !grouped.has(s.field))];
}

/** Rows whose text contains the filter text of every filtered column, ignoring case. */
export function filterItems<T>(
  items: readonly GridItem<T>[],
  columns: readonly GridColumn<T>[],
  filter: Record<string, string>,
): GridItem<T>[] {
  const tests = columns
    .filter(column => (filter[column.field] ?? '').trim() !== '')
    .map(column => ({ column, needle: filter[column.field].trim().toLowerCase() }));
  if (tests.length === 0) {
    return [...items];
  }
  return items.filter(item =>
    tests.every(({ column, needle }) => columnText(column, columnValue(column, item.row)).toLowerCase().includes(needle)),
  );
}

/** A stable sort: rows that compare equal keep their order. */
export function sortItems<T>(
  items: readonly GridItem<T>[],
  columns: readonly GridColumn<T>[],
  sort: readonly SortDescriptor[],
): GridItem<T>[] {
  const keys = sort
    .map(s => ({ column: columns.find(c => c.field === s.field), sign: s.dir === 'desc' ? -1 : 1 }))
    .filter((k): k is { column: GridColumn<T>; sign: number } => k.column !== undefined)
    .map(k => ({ ...k, compare: compareBy(k.column) }));
  if (keys.length === 0) {
    return [...items];
  }
  const values = new Map(items.map(item => [item, keys.map(k => columnValue(k.column, item.row))]));
  return [...items].sort((a, b) => {
    const va = values.get(a)!;
    const vb = values.get(b)!;
    for (let i = 0; i < keys.length; i++) {
      const order = keys[i].compare(va[i], vb[i]);
      if (order !== 0) {
        return order * keys[i].sign;
      }
    }
    return a.index - b.index;
  });
}

/**
 * The rows to render: a header for each run of rows with the same value of a group field, nested in the order of
 * `group`, then the rows. Rows must already be in group order (see effectiveSort). The rows of a collapsed group are
 * left out, its nested groups too.
 */
export function buildView<T>(
  items: readonly GridItem<T>[],
  columns: readonly GridColumn<T>[],
  group: readonly GroupDescriptor[],
  collapsed: ReadonlySet<string>,
  firstPosition = 0,
): GridViewRow<T>[] {
  const levels = group
    .map(g => columns.find(c => c.field === g.field))
    .filter((c): c is GridColumn<T> => c !== undefined)
    .map(column => ({ column, compare: compareBy(column) }));
  const out: GridViewRow<T>[] = [];
  const dataRow = (item: GridItem<T>, at: number): GridDataRow<T> => ({
    kind: 'data',
    key: `r${item.index}`,
    row: item.row,
    index: item.index,
    position: firstPosition + at,
  });
  if (levels.length === 0) {
    items.forEach((item, at) => out.push(dataRow(item, at)));
    return out;
  }
  const emit = (from: number, to: number, level: number, path: string): void => {
    const { column, compare } = levels[level];
    let start = from;
    while (start < to) {
      const value = columnValue(column, items[start].row);
      let end = start + 1;
      while (end < to && compare(columnValue(column, items[end].row), value) === 0) {
        end++;
      }
      const text = columnText(column, value);
      // The index keeps two runs of one value apart (possible when the host did not sort by the group).
      const key = `${path}\u0001${level}:${start}:${text}`;
      const header: GridGroupRow = {
        kind: 'group',
        key,
        level,
        field: column.field,
        title: columnTitle(column),
        value,
        text,
        count: end - start,
        collapsed: collapsed.has(key),
      };
      out.push(header);
      if (!header.collapsed) {
        if (level + 1 < levels.length) {
          emit(start, end, level + 1, key);
        } else {
          for (let i = start; i < end; i++) {
            out.push(dataRow(items[i], i));
          }
        }
      }
      start = end;
    }
  };
  emit(0, items.length, 0, '');
  return out;
}

/** `list` with `item` moved from `from` to before the element at `to` (indices of the list before the move). */
export function moveItem<T>(list: readonly T[], from: number, to: number): T[] {
  const out = [...list];
  const [item] = out.splice(from, 1);
  out.splice(to > from ? to - 1 : to, 0, item);
  return out;
}
