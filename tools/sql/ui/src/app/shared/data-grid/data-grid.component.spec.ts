import { Component, provideZonelessChangeDetection, signal } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { MatIconModule } from '@angular/material/icon';

import { settle } from '../../testing/test-helpers';
import { DataGridComponent } from './data-grid.component';
import { GridCellDirective } from './data-grid.directives';
import { GridColumn, GridState, GroupDescriptor, SortDescriptor } from './data-grid.models';

interface Person {
  name: string;
  team: string;
  score: number;
}

const PEOPLE: Person[] = [
  { name: 'Ada', team: 'red', score: 3 },
  { name: 'Bob', team: 'blue', score: 1 },
  { name: 'Cyd', team: 'red', score: 1 },
  { name: 'Dee', team: 'blue', score: 2 },
  { name: 'Eve', team: 'green', score: 5 },
];

@Component({
  imports: [DataGridComponent, GridCellDirective],
  template: `
    <app-data-grid
      style="display: flex; width: 900px; height: 600px"
      [data]="data()"
      [columns]="columns()"
      [mode]="mode()"
      [total]="data().length"
      [filterable]="true"
      [rowNumbers]="true"
      [pageable]="pageable()"
      selectable="multiple"
      [(sort)]="sort"
      [(group)]="group"
      [(pageSize)]="pageSize"
      [(selection)]="selection"
      (stateChange)="states.push($event)"
    >
      @if (custom()) {
        <ng-template gridCell let-value let-column="column">[{{ column.field }}={{ value }}]</ng-template>
      }
    </app-data-grid>
  `,
})
class HostComponent {
  readonly data = signal(PEOPLE);
  readonly columns = signal<GridColumn<Person>[]>([
    { field: 'name', title: 'Name' },
    { field: 'team', title: 'Team' },
    { field: 'score', title: 'Score' },
  ]);
  readonly mode = signal<'client' | 'server'>('client');
  readonly pageable = signal(true);
  readonly custom = signal(false);
  readonly sort = signal<SortDescriptor[]>([]);
  readonly group = signal<GroupDescriptor[]>([]);
  readonly pageSize = signal(100);
  readonly selection = signal<ReadonlySet<number>>(new Set());
  readonly states: GridState[] = [];
}

interface Contact {
  id: number;
  name: string;
  avatar: string;
  online: boolean;
}

const avatar = (hue: number) =>
  'data:image/svg+xml,' +
  encodeURIComponent(
    `<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32"><circle cx="16" cy="16" r="16" fill="hsl(${hue},60%,60%)"/></svg>`,
  );

/** Many rows, with an image and an icon in cells. */
@Component({
  imports: [DataGridComponent, GridCellDirective, MatIconModule],
  template: `
    <app-data-grid style="display: flex; width: 700px; height: 400px" [data]="contacts" [columns]="columns" [rowHeight]="48">
      <ng-template gridCell let-value let-row="row" let-column="column" let-text="text">
        @switch (column.field) {
          @case ('name') {
            <img class="avatar" [src]="row.avatar" alt="" width="32" height="32" decoding="async" />
            {{ text }}
          }
          @case ('online') {
            <mat-icon class="presence">{{ value ? 'check_circle' : 'do_not_disturb_on' }}</mat-icon>
          }
          @default {
            {{ text }}
          }
        }
      </ng-template>
    </app-data-grid>
  `,
})
class ContactsHostComponent {
  readonly contacts: Contact[] = Array.from({ length: 10000 }, (_, i) => ({
    id: i + 1,
    name: `Contact ${i + 1}`,
    avatar: avatar((i * 37) % 360),
    online: i % 3 === 0,
  }));
  readonly columns: GridColumn<Contact>[] = [
    { field: 'id', title: 'ID', width: 80 },
    { field: 'name', title: 'Name', width: 240 },
    { field: 'online', title: 'Online', width: 96, text: value => (value ? 'online' : 'offline') },
  ];
}

describe('DataGridComponent', () => {
  beforeEach(() => {
    TestBed.configureTestingModule({ providers: [provideZonelessChangeDetection()] });
  });

  async function create(setup: (host: HostComponent) => void = () => undefined) {
    const fixture = TestBed.createComponent(HostComponent);
    setup(fixture.componentInstance);
    document.body.appendChild(fixture.nativeElement);
    await fixture.whenStable();
    return { fixture, host: fixture.componentInstance, el: fixture.nativeElement as HTMLElement };
  }

  afterEach(() => document.querySelectorAll('.cdk-overlay-container').forEach(overlay => (overlay.innerHTML = '')));

  const headers = (el: HTMLElement) => Array.from(el.querySelectorAll<HTMLElement>('th.grid-header'));
  const titles = (el: HTMLElement) => headers(el).map(th => th.querySelector('.title')!.textContent!.trim());
  const header = (el: HTMLElement, title: string) => headers(el).find(th => th.querySelector('.title')!.textContent!.trim() === title)!;
  const column = (el: HTMLElement, title: string) => {
    const at = titles(el).indexOf(title);
    return Array.from(el.querySelectorAll('tr.grid-row')).map(row =>
      Array.from(row.querySelectorAll('td.grid-cell'))[at].textContent!.trim(),
    );
  };
  const groupRows = (el: HTMLElement) =>
    Array.from(el.querySelectorAll('tr.grid-group-row')).map(
      row =>
        `${row.querySelector('.grid-group-title')!.textContent} ${row.querySelector('.grid-group-value')!.textContent} ${row.querySelector('.grid-group-count')!.textContent}`,
    );
  const chips = (el: HTMLElement) => Array.from(el.querySelectorAll('.grid-group-chip')).map(chip => chip.getAttribute('data-field'));

  function center(target: Element): { x: number; y: number } {
    const box = target.getBoundingClientRect();
    return { x: box.left + box.width / 2, y: box.top + box.height / 2 };
  }

  /** Presses on `from`, moves in steps to `to` and lets go there. */
  async function dragTo(from: Element, to: { x: number; y: number }) {
    const start = center(from);
    from.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, button: 0, clientX: start.x, clientY: start.y }));
    for (const step of [0.25, 0.5, 1]) {
      document.dispatchEvent(
        new PointerEvent('pointermove', {
          bubbles: true,
          clientX: start.x + (to.x - start.x) * step,
          clientY: start.y + (to.y - start.y) * step,
        }),
      );
    }
    await settle();
    document.dispatchEvent(new PointerEvent('pointerup', { bubbles: true, clientX: to.x, clientY: to.y }));
    from.dispatchEvent(new MouseEvent('click', { bubbles: true }));
    await settle();
  }

  it('shows the columns, the row numbers and the values', async () => {
    const { el } = await create();
    expect(titles(el)).toEqual(['Name', 'Team', 'Score']);
    expect(column(el, 'Name')).toEqual(['Ada', 'Bob', 'Cyd', 'Dee', 'Eve']);
    expect(Array.from(el.querySelectorAll('tr.grid-row td.rowno')).map(td => td.textContent!.trim())).toEqual(['1', '2', '3', '4', '5']);
    expect(el.querySelector('.grid-group-hint')!.textContent).toContain('Drag a column header');
  });

  it('renders cells with the template it is given', async () => {
    const { el } = await create(host => host.custom.set(true));
    expect(column(el, 'Score')[0]).toBe('[score=3]');
  });

  describe('sorting', () => {
    it('goes from ascending to descending to unsorted', async () => {
      const { el, host } = await create();
      header(el, 'Score').click();
      await settle();
      expect(column(el, 'Name')).toEqual(['Bob', 'Cyd', 'Dee', 'Ada', 'Eve']);
      expect(header(el, 'Score').getAttribute('aria-sort')).toBe('ascending');
      header(el, 'Score').click();
      await settle();
      expect(column(el, 'Name')).toEqual(['Eve', 'Ada', 'Dee', 'Bob', 'Cyd']);
      expect(host.sort()).toEqual([{ field: 'score', dir: 'desc' }]);
      header(el, 'Score').click();
      await settle();
      expect(column(el, 'Name')).toEqual(['Ada', 'Bob', 'Cyd', 'Dee', 'Eve']);
      expect(header(el, 'Score').querySelector('.sort')).toBeNull();
    });

    it('adds a column to the sort with Ctrl and numbers the sorted columns', async () => {
      const { el, host } = await create();
      header(el, 'Score').click();
      header(el, 'Name').dispatchEvent(new MouseEvent('click', { bubbles: true, ctrlKey: true }));
      header(el, 'Name').dispatchEvent(new MouseEvent('click', { bubbles: true, ctrlKey: true }));
      await settle();
      expect(host.sort()).toEqual([
        { field: 'score', dir: 'asc' },
        { field: 'name', dir: 'desc' },
      ]);
      expect(column(el, 'Name')).toEqual(['Cyd', 'Bob', 'Dee', 'Ada', 'Eve']);
      expect(headers(el).map(th => th.querySelector('.sort-order')?.textContent ?? '')).toEqual(['2', '', '1']);
      header(el, 'Team').click();
      await settle();
      expect(host.sort()).toEqual([{ field: 'team', dir: 'asc' }]);
    });

    it('sorts from the keyboard', async () => {
      const { el, host } = await create();
      header(el, 'Name').dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
      await settle();
      expect(host.sort()).toEqual([{ field: 'name', dir: 'asc' }]);
    });
  });

  describe('grouping', () => {
    it('groups by a column whose header is dropped on the group panel', async () => {
      const { el, host } = await create();
      await dragTo(header(el, 'Team'), center(el.querySelector('.grid-group-panel')!));
      expect(host.group()).toEqual([{ field: 'team', dir: 'asc' }]);
      expect(chips(el)).toEqual(['team']);
      expect(groupRows(el)).toEqual(['Team: blue (2)', 'Team: green (1)', 'Team: red (2)']);
      expect(column(el, 'Name')).toEqual(['Bob', 'Dee', 'Eve', 'Ada', 'Cyd']);
      expect(el.querySelectorAll('tr.grid-row td.grid-indent').length).toBe(5);
      // The drag did not sort as well.
      expect(host.sort()).toEqual([]);
    });

    it('nests groups in the order of the chips and sorts inside them', async () => {
      const { el, host } = await create();
      header(el, 'Name').click();
      await dragTo(header(el, 'Team'), center(el.querySelector('.grid-group-panel')!));
      const chip = el.querySelector('.grid-group-chip')!.getBoundingClientRect();
      // Before the first chip.
      await dragTo(header(el, 'Score'), { x: chip.left + 2, y: chip.top + chip.height / 2 });
      expect(host.group().map(g => g.field)).toEqual(['score', 'team']);
      expect(groupRows(el)).toEqual([
        'Score: 1 (2)',
        'Team: blue (1)',
        'Team: red (1)',
        'Score: 2 (1)',
        'Team: blue (1)',
        'Score: 3 (1)',
        'Team: red (1)',
        'Score: 5 (1)',
        'Team: green (1)',
      ]);
    });

    it('turns the order of a group around from its chip and from its header', async () => {
      const { el, host } = await create(h => h.group.set([{ field: 'team', dir: 'asc' }]));
      (el.querySelector('.grid-group-chip') as HTMLElement).click();
      await settle();
      expect(host.group()).toEqual([{ field: 'team', dir: 'desc' }]);
      expect(groupRows(el)[0]).toBe('Team: red (2)');
      header(el, 'Team').click();
      await settle();
      expect(host.group()).toEqual([{ field: 'team', dir: 'asc' }]);
    });

    it('collapses and expands a group', async () => {
      const { el } = await create(h => h.group.set([{ field: 'team', dir: 'asc' }]));
      el.querySelectorAll<HTMLElement>('.grid-group-toggle')[0].click();
      await settle();
      expect(column(el, 'Name')).toEqual(['Eve', 'Ada', 'Cyd']);
      el.querySelectorAll<HTMLElement>('.grid-group-toggle')[0].click();
      await settle();
      expect(column(el, 'Name')).toEqual(['Bob', 'Dee', 'Eve', 'Ada', 'Cyd']);
    });

    it('ungroups by the chip button and by dragging the chip away', async () => {
      const { el, host } = await create(h =>
        h.group.set([
          { field: 'team', dir: 'asc' },
          { field: 'score', dir: 'asc' },
        ]),
      );
      el.querySelector<HTMLElement>('.grid-group-chip .grid-chip-remove')!.click();
      await settle();
      expect(host.group()).toEqual([{ field: 'score', dir: 'asc' }]);
      const rows = el.querySelector('.grid-scroll')!.getBoundingClientRect();
      await dragTo(el.querySelector('.grid-group-chip')!, { x: rows.left + 100, y: rows.bottom - 20 });
      expect(host.group()).toEqual([]);
      expect(el.querySelectorAll('tr.grid-group-row').length).toBe(0);
    });

    it('reorders groups by dragging their chips', async () => {
      const { el, host } = await create(h =>
        h.group.set([
          { field: 'team', dir: 'asc' },
          { field: 'score', dir: 'asc' },
        ]),
      );
      const first = el.querySelectorAll('.grid-group-chip')[0].getBoundingClientRect();
      await dragTo(el.querySelectorAll('.grid-group-chip')[1], { x: first.left + 2, y: first.top + first.height / 2 });
      expect(host.group().map(g => g.field)).toEqual(['score', 'team']);
    });

    it('groups from the column menu', async () => {
      const { el, host } = await create();
      header(el, 'Team').querySelector<HTMLElement>('.grid-menu-button')!.click();
      await settle();
      document.querySelector<HTMLElement>('.grid-group-by')!.click();
      await settle();
      expect(host.group()).toEqual([{ field: 'team', dir: 'asc' }]);
    });
  });

  describe('columns', () => {
    it('moves a column whose header is dropped between two others', async () => {
      const { el } = await create();
      const team = header(el, 'Team').getBoundingClientRect();
      await dragTo(header(el, 'Score'), { x: team.left + 4, y: team.top + team.height / 2 });
      expect(titles(el)).toEqual(['Name', 'Score', 'Team']);
      expect(column(el, 'Score')).toEqual(['3', '1', '1', '2', '5']);
    });

    it('keeps the arrangement when the same columns come back', async () => {
      const { el, host, fixture } = await create();
      const name = header(el, 'Name').getBoundingClientRect();
      await dragTo(header(el, 'Team'), { x: name.left + 4, y: name.top + name.height / 2 });
      host.columns.set(host.columns().map(c => ({ ...c })));
      host.data.set(PEOPLE.slice(0, 2));
      fixture.detectChanges();
      await settle();
      expect(titles(el)).toEqual(['Team', 'Name', 'Score']);
      expect(column(el, 'Name')).toEqual(['Ada', 'Bob']);
    });

    it('resizes a column by its edge', async () => {
      const { el } = await create();
      const before = header(el, 'Name').getBoundingClientRect().width;
      const edge = header(el, 'Name').querySelector('.grid-resize')!;
      const start = center(edge);
      edge.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, button: 0, clientX: start.x, clientY: start.y }));
      document.dispatchEvent(new PointerEvent('pointermove', { clientX: start.x + 80, clientY: start.y }));
      document.dispatchEvent(new PointerEvent('pointerup', { clientX: start.x + 80, clientY: start.y }));
      await settle();
      expect(Math.abs(header(el, 'Name').getBoundingClientRect().width - (before + 80))).toBeLessThan(2);
    });

    it('hides a column from the column menu but never the last one', async () => {
      const { el } = await create();
      header(el, 'Team').querySelector<HTMLElement>('.grid-menu-button')!.click();
      await settle();
      document.querySelector<HTMLElement>('.grid-columns')!.click();
      await settle();
      const items = () => Array.from(document.querySelectorAll<HTMLButtonElement>('[role="menuitemcheckbox"]'));
      items()[1].click();
      await settle();
      expect(titles(el)).toEqual(['Name', 'Score']);
      items()[0].click();
      await settle();
      expect(titles(el)).toEqual(['Score']);
      expect(items()[2].disabled).toBeTrue();
    });
  });

  it('filters the rows by the text of a column', async () => {
    const { el, host } = await create();
    const box = header(el, 'Name').closest('table')!.querySelectorAll<HTMLInputElement>('input.filter')[1];
    box.value = 'RED';
    box.dispatchEvent(new Event('input'));
    await settle();
    expect(column(el, 'Name')).toEqual(['Ada', 'Cyd']);
    expect(host.states.at(-1)!.filter).toEqual({ team: 'RED' });
  });

  it('pages the processed rows', async () => {
    const { el } = await create(h => h.pageSize.set(2));
    header(el, 'Name').click();
    header(el, 'Name').click();
    await settle();
    expect(column(el, 'Name')).toEqual(['Eve', 'Dee']);
    el.querySelector<HTMLButtonElement>('.mat-mdc-paginator-navigation-next')!.click();
    await settle();
    expect(column(el, 'Name')).toEqual(['Cyd', 'Bob']);
    expect(Array.from(el.querySelectorAll('tr.grid-row td.rowno')).map(td => td.textContent!.trim())).toEqual(['3', '4']);
  });

  it('selects rows with Ctrl and Shift in the order shown', async () => {
    const { el, host } = await create();
    header(el, 'Score').click();
    await settle();
    const numbers = () => Array.from(el.querySelectorAll<HTMLElement>('tr.grid-row td.rowno'));
    numbers()[0].click();
    numbers()[2].dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true }));
    await settle();
    // Bob, Cyd and Dee: rows 1, 2 and 3 of the data.
    expect([...host.selection()].sort()).toEqual([1, 2, 3]);
    numbers()[1].dispatchEvent(new MouseEvent('click', { bubbles: true, ctrlKey: true }));
    await settle();
    expect([...host.selection()].sort()).toEqual([1, 3]);
    expect(el.querySelectorAll('tr.grid-row.selected').length).toBe(2);
  });

  it('leaves the rows to the host in server mode', async () => {
    const { el, host } = await create(h => h.mode.set('server'));
    header(el, 'Score').click();
    await settle();
    expect(column(el, 'Name')).toEqual(['Ada', 'Bob', 'Cyd', 'Dee', 'Eve']);
    await dragTo(header(el, 'Team'), center(el.querySelector('.grid-group-panel')!));
    expect(host.states.at(-1)).toEqual({
      skip: 0,
      take: 100,
      sort: [{ field: 'score', dir: 'asc' }],
      group: [{ field: 'team', dir: 'asc' }],
      filter: {},
    });
    // Runs of equal values, as the page came.
    expect(groupRows(el)).toEqual(['Team: red (1)', 'Team: blue (1)', 'Team: red (1)', 'Team: blue (1)', 'Team: green (1)']);
  });

  it('fits a column to its content', async () => {
    const { el } = await create();
    header(el, 'Name')
      .querySelector<HTMLElement>('.grid-resize')!
      .dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
    await settle();
    const width = header(el, 'Name').getBoundingClientRect().width;
    expect(width).toBeLessThan(160);
    expect(width).toBeGreaterThan(48);
  });

  describe('virtual scrolling', () => {
    async function createContacts() {
      const fixture = TestBed.createComponent(ContactsHostComponent);
      document.body.appendChild(fixture.nativeElement);
      await fixture.whenStable();
      await new Promise(resolve => requestAnimationFrame(resolve));
      await settle();
      return fixture.nativeElement as HTMLElement;
    }

    const shown = (el: HTMLElement) => Array.from(el.querySelectorAll<HTMLElement>('tr.grid-row'));

    it('renders only the rows in view, at the row height', async () => {
      const el = await createContacts();
      const rows = shown(el);
      expect(rows.length).toBeGreaterThan(5);
      expect(rows.length).toBeLessThan(40);
      expect(rows.every(row => row.offsetHeight === 48)).toBeTrue();
      const scroller = el.querySelector<HTMLElement>('.grid-scroll')!;
      expect(scroller.scrollHeight).toBeGreaterThan(10000 * 48);
    });

    it('follows the scroll', async () => {
      const el = await createContacts();
      const scroller = el.querySelector<HTMLElement>('.grid-scroll')!;
      scroller.scrollTop = 5000 * 48;
      scroller.dispatchEvent(new Event('scroll'));
      await settle();
      const ids = shown(el).map(row => Number(row.querySelector('td.grid-cell')!.textContent));
      expect(ids).toContain(5001);
      expect(ids.length).toBeLessThan(40);
      // What is shown lies where the scroll put it.
      const row = shown(el).find(r => r.querySelector('td.grid-cell')!.textContent!.trim() === '5003')!;
      const offset = row.getBoundingClientRect().top - scroller.getBoundingClientRect().top;
      expect(offset).toBeGreaterThan(0);
      expect(offset).toBeLessThan(400);
    });

    it('shows images and icons in cells', async () => {
      const el = await createContacts();
      const first = shown(el)[0];
      expect(first.querySelector<HTMLImageElement>('img.avatar')!.src).toContain('data:image/svg+xml');
      expect(first.querySelector('.presence')!.textContent).toBe('check_circle');
      expect(shown(el)[1].querySelector('.presence')!.textContent).toBe('do_not_disturb_on');
      expect(el.querySelectorAll('img.avatar').length).toBe(shown(el).length);
    });
  });
});
