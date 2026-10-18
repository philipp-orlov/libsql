import { buildView, defaultCompare, effectiveSort, filterItems, moveItem, sortItems } from './data-grid.process';
import { GridColumn, GridGroupRow } from './data-grid.models';

interface Person {
  name: string;
  team: string | null;
  score: number;
}

const PEOPLE: Person[] = [
  { name: 'Ada', team: 'red', score: 3 },
  { name: 'bob', team: 'blue', score: 1 },
  { name: 'Cyd', team: 'red', score: 1 },
  { name: 'Dee', team: null, score: 2 },
];
const COLUMNS: GridColumn<Person>[] = [{ field: 'name' }, { field: 'team', title: 'Team' }, { field: 'score' }];
const items = () => PEOPLE.map((row, index) => ({ row, index }));
const names = (list: { row: Person }[]) => list.map(item => item.row.name);

describe('data grid processing', () => {
  it('orders empty values first, then numbers, then text', () => {
    const values = ['b', 2, null, 'a', 10];
    expect([...values].sort(defaultCompare)).toEqual([null, 2, 10, 'a', 'b']);
    expect(defaultCompare(undefined, null)).toBe(0);
  });

  it('sorts by several fields and keeps the order of equal rows', () => {
    expect(names(sortItems(items(), COLUMNS, [{ field: 'score', dir: 'asc' }]))).toEqual(['bob', 'Cyd', 'Dee', 'Ada']);
    expect(
      names(
        sortItems(items(), COLUMNS, [
          { field: 'score', dir: 'asc' },
          { field: 'name', dir: 'desc' },
        ]),
      ),
    ).toEqual(['bob', 'Cyd', 'Dee', 'Ada']);
    expect(names(sortItems(items(), COLUMNS, [{ field: 'name', dir: 'desc' }]))).toEqual(['bob', 'Dee', 'Cyd', 'Ada']);
  });

  it('uses the compare of a column', () => {
    const caseless: GridColumn<Person>[] = [
      { field: 'name', compare: (a, b) => String(a).localeCompare(String(b), 'en', { sensitivity: 'base' }) },
    ];
    expect(names(sortItems(items(), caseless, [{ field: 'name', dir: 'asc' }]))).toEqual(['Ada', 'bob', 'Cyd', 'Dee']);
  });

  it('sorts by the groups first, then by the rest of the sort', () => {
    expect(
      effectiveSort(
        [{ field: 'team', dir: 'desc' }],
        [
          { field: 'team', dir: 'asc' },
          { field: 'name', dir: 'asc' },
        ],
      ),
    ).toEqual([
      { field: 'team', dir: 'desc' },
      { field: 'name', dir: 'asc' },
    ]);
  });

  it('keeps the rows that contain the text of every filter, ignoring case', () => {
    expect(names(filterItems(items(), COLUMNS, { name: 'D' }))).toEqual(['Ada', 'Cyd', 'Dee']);
    expect(names(filterItems(items(), COLUMNS, { name: 'd', team: 'RE' }))).toEqual(['Ada', 'Cyd']);
    expect(names(filterItems(items(), COLUMNS, { team: '  ' }))).toEqual(['Ada', 'bob', 'Cyd', 'Dee']);
  });

  it('heads each run of a group value and nests the next group inside', () => {
    const group = [
      { field: 'team', dir: 'asc' as const },
      { field: 'score', dir: 'asc' as const },
    ];
    const sorted = sortItems(items(), COLUMNS, effectiveSort(group, []));
    const view = buildView(sorted, COLUMNS, group, new Set());
    expect(
      view.map(row =>
        row.kind === 'group' ? `${'  '.repeat(row.level)}${row.title}: ${row.text} (${row.count})` : `${'  '.repeat(2)}${row.row.name}`,
      ),
    ).toEqual([
      'Team:  (1)',
      '  score: 2 (1)',
      '    Dee',
      'Team: blue (1)',
      '  score: 1 (1)',
      '    bob',
      'Team: red (2)',
      '  score: 1 (1)',
      '    Cyd',
      '  score: 3 (1)',
      '    Ada',
    ]);
  });

  it('leaves out what a collapsed group holds', () => {
    const group = [{ field: 'team', dir: 'asc' as const }];
    const sorted = sortItems(items(), COLUMNS, effectiveSort(group, []));
    const red = (buildView(sorted, COLUMNS, group, new Set()).find(row => row.kind === 'group' && row.text === 'red') as GridGroupRow).key;
    const view = buildView(sorted, COLUMNS, group, new Set([red]));
    expect(view.filter(row => row.kind === 'data').map(row => (row.kind === 'data' ? row.row.name : ''))).toEqual(['Dee', 'bob']);
    expect((view.find(row => row.kind === 'group' && row.text === 'red') as GridGroupRow).collapsed).toBeTrue();
  });

  it('numbers the rows from the first position', () => {
    const view = buildView(items().slice(0, 2), COLUMNS, [], new Set(), 10);
    expect(view.map(row => (row.kind === 'data' ? row.position : -1))).toEqual([10, 11]);
  });

  it('moves an item to before another', () => {
    expect(moveItem(['a', 'b', 'c', 'd'], 0, 3)).toEqual(['b', 'c', 'a', 'd']);
    expect(moveItem(['a', 'b', 'c', 'd'], 3, 1)).toEqual(['a', 'd', 'b', 'c']);
    expect(moveItem(['a', 'b', 'c', 'd'], 1, 4)).toEqual(['a', 'c', 'd', 'b']);
  });
});
