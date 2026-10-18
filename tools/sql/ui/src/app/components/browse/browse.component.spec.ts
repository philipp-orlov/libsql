import { provideZonelessChangeDetection } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { MatPaginator } from '@angular/material/paginator';
import { By } from '@angular/platform-browser';

import { WorkspaceService } from '../../services/workspace.service';
import { DataGridComponent } from '../../shared/data-grid';
import {
  FakeDatabase,
  PERSON_COLUMNS,
  databaseProviders,
  dialogAnswering,
  fakeDatabase,
  fakeNotice,
  personPage,
  query,
  settle,
  typeInto,
} from '../../testing/test-helpers';
import { ConfirmDialogComponent } from '../confirm-dialog/confirm-dialog.component';
import { RecordDialogComponent } from '../record-dialog/record-dialog.component';
import { BrowseComponent } from './browse.component';

describe('BrowseComponent', () => {
  let db: FakeDatabase;
  let notice: ReturnType<typeof fakeNotice>;
  let dialog: ReturnType<typeof dialogAnswering>;
  let workspace: WorkspaceService;

  beforeEach(() => {
    db = fakeDatabase();
    notice = fakeNotice();
    dialog = dialogAnswering(undefined);
    TestBed.configureTestingModule({ providers: [provideZonelessChangeDetection(), ...databaseProviders(db, notice), dialog.provider] });
    workspace = TestBed.inject(WorkspaceService);
  });

  async function create(table: string | null = 'person') {
    workspace.table.set(table);
    const fixture = TestBed.createComponent(BrowseComponent);
    await fixture.whenStable();
    return { fixture, el: fixture.nativeElement as HTMLElement };
  }

  const dataRows = (el: HTMLElement) => Array.from(el.querySelectorAll('tr[mat-row]'));
  const rowTexts = (el: HTMLElement) => dataRows(el).map(r => Array.from(r.querySelectorAll('td')).map(td => td.textContent!.trim()).join(' '));
  const cell = (el: HTMLElement, row: number, column: number) => dataRows(el)[row].querySelectorAll<HTMLElement>('td.mat-mdc-cell')[column + 1];
  const button = (el: HTMLElement, name: string) => query<HTMLButtonElement>(el, `button.${name}`);
  const doubleClick = (target: HTMLElement) => target.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
  const pageTo = (fixture: ComponentFixture<BrowseComponent>, pageIndex: number, pageSize: number) =>
    fixture.debugElement.query(By.directive(MatPaginator)).componentInstance.page.emit({ pageIndex, pageSize, length: 500, previousPageIndex: 0 });

  it('shows nothing until a table is chosen', async () => {
    const { el } = await create(null);
    expect(db.browse).not.toHaveBeenCalled();
    expect(query(el, '.hint').textContent).toContain('Select a table');
  });

  it('loads the first page of the chosen table', async () => {
    await create();
    expect(db.browse).toHaveBeenCalledOnceWith({ table: 'person', offset: 0, limit: 100, sort: [], filters: {} });
  });

  it('lists the tables and then the views to choose from', async () => {
    const { fixture } = await create();
    expect(fixture.componentInstance['tableNames']()).toEqual(['person', 'tag', 'adults']);
  });

  it('shows the column names, the row numbers and the values', async () => {
    const { el } = await create();
    expect(Array.from(el.querySelectorAll('th.sortable .title')).map(title => title.textContent!.trim())).toEqual(['id', 'name', 'age', 'note']);
    expect(rowTexts(el)).toEqual(['1 1 Ada 36 first', '2 2 Bob 17 NULL', '3 3 Cyd 52 third']);
    expect(el.querySelectorAll('.placeholder').length).toBe(1);
  });

  it('numbers the rows from the start of the page', async () => {
    db.browse.and.callFake(async () => personPage({ offset: 200, rows: [[7, 'Gus', 20, null]], keys: [[7]], total: 201 }));
    const { el } = await create();
    expect(dataRows(el)[0].querySelector('td.rowno')!.textContent).toBe('201');
  });

  it('shows a blob as a label and an oversized integer in full', async () => {
    db.browse.and.callFake(async () =>
      personPage({ rows: [[{ type: 'integer', value: '9007199254740993' }, 'x', 1, { type: 'blob', size: 12, value: '00' }]], keys: [[1]] }),
    );
    const { el } = await create();
    expect(rowTexts(el)[0]).toBe('1 9007199254740993 x 1 BLOB (12 bytes)');
  });

  describe('sorting', () => {
    it('goes from ascending to descending to the table order', async () => {
      const { el } = await create();
      const header = el.querySelectorAll<HTMLElement>('th.sortable')[1];
      header.click();
      await settle();
      expect(db.browse.calls.mostRecent().args[0].sort).toEqual([{ column: 'name', descending: false }]);
      expect(el.querySelector('th .sort')!.textContent).toBe('arrow_upward');

      el.querySelectorAll<HTMLElement>('th.sortable')[1].click();
      await settle();
      expect(db.browse.calls.mostRecent().args[0].sort).toEqual([{ column: 'name', descending: true }]);
      expect(el.querySelector('th .sort')!.textContent).toBe('arrow_downward');

      el.querySelectorAll<HTMLElement>('th.sortable')[1].click();
      await settle();
      expect(db.browse.calls.mostRecent().args[0].sort).toEqual([]);
      expect(el.querySelector('th .sort')).toBeNull();
    });

    it('sorts by one column at a time and starts again on the first page', async () => {
      const { fixture, el } = await create();
      pageTo(fixture, 2, 50);
      await settle();
      el.querySelectorAll<HTMLElement>('th.sortable')[1].click();
      await settle();
      el.querySelectorAll<HTMLElement>('th.sortable')[2].click();
      await settle();
      const request = db.browse.calls.mostRecent().args[0];
      expect(request.sort).toEqual([{ column: 'age', descending: false }]);
      expect(request.offset).toBe(0);
    });
  });

  describe('grouping', () => {
    const groupHeaders = (el: HTMLElement) =>
      Array.from(el.querySelectorAll('tr.grid-group-row')).map(row => `${row.querySelector('.grid-group-value')!.textContent} ${row.querySelector('.grid-group-count')!.textContent}`);

    it('asks for the rows by the grouped column first and heads each group', async () => {
      db.browse.and.callFake(async () => personPage({ rows: [[2, 'Bob', 17, null], [1, 'Ada', 36, 'first'], [3, 'Cyd', 36, 'third']], keys: [[2], [1], [3]] }));
      const { fixture, el } = await create();
      el.querySelectorAll<HTMLElement>('th.sortable')[1].click();
      await settle();
      fixture.debugElement.query(By.directive(DataGridComponent)).componentInstance['addGroup']('age');
      await settle();
      const request = db.browse.calls.mostRecent().args[0];
      expect(request.sort).toEqual([
        { column: 'age', descending: false },
        { column: 'name', descending: false },
      ]);
      expect(request.offset).toBe(0);
      expect(groupHeaders(el)).toEqual(['17 (1)', '36 (2)']);
    });

    it('deletes the rows selected under a group by their keys', async () => {
      dialog.answer(true);
      db.browse.and.callFake(async () => personPage({ rows: [[2, 'Bob', 17, null], [1, 'Ada', 36, 'first'], [3, 'Cyd', 36, 'third']], keys: [[2], [1], [3]] }));
      const { fixture, el } = await create();
      fixture.debugElement.query(By.directive(DataGridComponent)).componentInstance['addGroup']('age');
      await settle();
      el.querySelectorAll<HTMLElement>('tr.grid-row td.rowno')[2].click();
      await settle();
      button(el, 'delete-record').click();
      await settle();
      expect(db.deleteRows).toHaveBeenCalledOnceWith('person', [[3]]);
    });
  });

  describe('filtering', () => {
    it('reloads a little after the typing stops, from the first page', async () => {
      const { el } = await create();
      const boxes = el.querySelectorAll<HTMLInputElement>('input.filter');
      typeInto(boxes[1], 'a');
      typeInto(boxes[1], 'ad');
      expect(db.browse).toHaveBeenCalledTimes(1);
      await new Promise(resolve => setTimeout(resolve, 400));
      await settle();
      expect(db.browse).toHaveBeenCalledTimes(2);
      expect(db.browse.calls.mostRecent().args[0].filters).toEqual({ name: 'ad' });
      expect(db.browse.calls.mostRecent().args[0].offset).toBe(0);
    });

    it('drops the filter of a box that is emptied', async () => {
      const { el } = await create();
      const boxes = el.querySelectorAll<HTMLInputElement>('input.filter');
      typeInto(boxes[2], '>30');
      typeInto(boxes[2], '  ');
      await new Promise(resolve => setTimeout(resolve, 400));
      expect(db.browse.calls.mostRecent().args[0].filters).toEqual({});
    });
  });

  it('pages', async () => {
    const { fixture } = await create();
    pageTo(fixture, 3, 50);
    await settle();
    expect(db.browse.calls.mostRecent().args[0]).toEqual(jasmine.objectContaining({ offset: 150, limit: 50 }));
  });

  it('starts over when another table is chosen', async () => {
    const { fixture, el } = await create();
    el.querySelectorAll<HTMLElement>('th.sortable')[1].click();
    await settle();
    fixture.componentInstance['selectTable']('tag');
    await settle();
    expect(db.browse.calls.mostRecent().args[0]).toEqual({ table: 'tag', offset: 0, limit: 100, sort: [], filters: {} });
  });

  it('loads again when the data may have changed', async () => {
    await create();
    db.revision.update(count => count + 1);
    await settle();
    expect(db.browse).toHaveBeenCalledTimes(2);
  });

  it('refreshes on demand', async () => {
    const { el } = await create();
    button(el, 'refresh').click();
    await settle();
    expect(db.browse).toHaveBeenCalledTimes(2);
  });

  it('reports a page that cannot be loaded and shows no stale rows', async () => {
    const { el } = await create();
    db.browse.and.rejectWith(new Error('no such table: person'));
    button(el, 'refresh').click();
    await settle();
    expect(notice.error).toHaveBeenCalledWith(jasmine.objectContaining({ message: 'no such table: person' }));
    expect(dataRows(el).length).toBe(0);
  });

  it('lets go of a table that was dropped', async () => {
    await create();
    db.schema.update(schema => ({ ...schema, tables: schema.tables.filter(table => table.name !== 'person') }));
    await settle();
    expect(workspace.table()).toBeNull();
  });

  describe('editing', () => {
    async function edit(el: HTMLElement, row: number, column: number, text: string, finish: (input: HTMLInputElement) => void) {
      cell(el, row, column).click();
      doubleClick(cell(el, row, column));
      await settle();
      const input = query<HTMLInputElement>(el, 'input.cell-input');
      typeInto(input, text);
      finish(input);
      await settle();
    }

    const enter = (input: HTMLInputElement) => input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));

    it('opens the cell for editing on a double click, with the value in it', async () => {
      const { el } = await create();
      doubleClick(cell(el, 0, 1));
      await settle();
      expect(query<HTMLInputElement>(el, 'input.cell-input').value).toBe('Ada');
    });

    it('updates the row by its key when Enter is pressed', async () => {
      const { el } = await create();
      await edit(el, 1, 1, 'Zed', enter);
      expect(db.updateRow).toHaveBeenCalledOnceWith('person', [2], { name: 'Zed' });
      expect(el.querySelector('input.cell-input')).toBeNull();
    });

    it('updates the row when the box loses the focus', async () => {
      const { el } = await create();
      await edit(el, 2, 1, 'Zed', input => input.dispatchEvent(new FocusEvent('blur')));
      expect(db.updateRow).toHaveBeenCalledOnceWith('person', [3], { name: 'Zed' });
    });

    it('drops the edit on Escape', async () => {
      const { el } = await create();
      await edit(el, 0, 1, 'Zed', input => {
        input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
        input.dispatchEvent(new FocusEvent('blur'));
      });
      expect(db.updateRow).not.toHaveBeenCalled();
      expect(el.querySelector('input.cell-input')).toBeNull();
    });

    it('sends nothing when the value is unchanged', async () => {
      const { el } = await create();
      await edit(el, 0, 1, 'Ada', enter);
      expect(db.updateRow).not.toHaveBeenCalled();
    });

    it('sends text typed into a number column as a number', async () => {
      const { el } = await create();
      await edit(el, 0, 2, '40', enter);
      expect(db.updateRow).toHaveBeenCalledOnceWith('person', [1], { age: 40 });
    });

    it('leaves a NULL alone when the box is left empty', async () => {
      const { el } = await create();
      await edit(el, 1, 3, '', enter);
      expect(db.updateRow).not.toHaveBeenCalled();
    });

    it('sets the active cell to NULL', async () => {
      const { el } = await create();
      cell(el, 0, 3).click();
      await settle();
      button(el, 'set-null').click();
      await settle();
      expect(db.updateRow).toHaveBeenCalledOnceWith('person', [1], { note: null });
    });

    it('reports an update that fails', async () => {
      db.updateRow.and.rejectWith(new Error('NOT NULL constraint failed'));
      const { el } = await create();
      await edit(el, 0, 1, '', enter);
      expect(notice.error).toHaveBeenCalledWith(jasmine.objectContaining({ message: 'NOT NULL constraint failed' }));
    });

    it('does not edit a blob', async () => {
      db.browse.and.callFake(async () => personPage({ rows: [[1, 'x', 1, { type: 'blob', size: 2, value: '00ff' }]], keys: [[1]] }));
      const { el } = await create();
      doubleClick(cell(el, 0, 3));
      await settle();
      expect(el.querySelector('input.cell-input')).toBeNull();
    });
  });

  describe('a table that cannot be edited', () => {
    beforeEach(() => {
      db.browse.and.callFake(async () => personPage({ editable: false, view: true, table: 'adults', keys: [[], [], []] }));
    });

    it('keeps every editing button off', async () => {
      const { el } = await create('adults');
      cell(el, 0, 0).click();
      await settle();
      expect(button(el, 'new-record').disabled).toBeTrue();
      expect(button(el, 'delete-record').disabled).toBeTrue();
      expect(button(el, 'set-null').disabled).toBeTrue();
    });

    it('does not open a cell for editing', async () => {
      const { el } = await create('adults');
      doubleClick(cell(el, 0, 1));
      await settle();
      expect(el.querySelector('input.cell-input')).toBeNull();
    });
  });

  describe('records', () => {
    it('selects rows and deletes them by key after a confirmation', async () => {
      dialog.answer(true);
      const { el } = await create();
      const numbers = dataRows(el).map(row => row.querySelector<HTMLElement>('td.rowno')!);
      expect(button(el, 'delete-record').disabled).toBeTrue();
      numbers[0].click();
      numbers[2].dispatchEvent(new MouseEvent('click', { bubbles: true, ctrlKey: true }));
      await settle();
      expect(dataRows(el).filter(row => row.classList.contains('selected')).length).toBe(2);

      button(el, 'delete-record').click();
      await settle();
      expect(dialog.open.calls.mostRecent().args[0]).toBe(ConfirmDialogComponent);
      expect(db.deleteRows).toHaveBeenCalledOnceWith('person', [[1], [3]]);
    });

    it('selects a range with Shift', async () => {
      const { el } = await create();
      const numbers = dataRows(el).map(row => row.querySelector<HTMLElement>('td.rowno')!);
      numbers[0].click();
      numbers[2].dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true }));
      await settle();
      expect(dataRows(el).filter(row => row.classList.contains('selected')).length).toBe(3);
    });

    it('deletes nothing when the confirmation is refused', async () => {
      const { el } = await create();
      dataRows(el)[0].querySelector<HTMLElement>('td.rowno')!.click();
      await settle();
      button(el, 'delete-record').click();
      await settle();
      expect(db.deleteRows).not.toHaveBeenCalled();
    });

    it('adds the record a dialog describes and goes to the last page', async () => {
      dialog.answer({ name: 'Eve', age: 30 });
      db.browse.and.callFake(async () => personPage({ total: 250 }));
      const { fixture, el } = await create();
      fixture.componentInstance['pageSize'].set(100);
      button(el, 'new-record').click();
      await settle();
      const [component, config] = dialog.open.calls.mostRecent().args;
      expect(component).toBe(RecordDialogComponent);
      expect(config.data.table).toBe('person');
      expect(config.data.columns).toEqual(PERSON_COLUMNS);
      expect(db.insertRow).toHaveBeenCalledOnceWith('person', { name: 'Eve', age: 30 });
      expect(db.browse.calls.mostRecent().args[0].offset).toBe(200);
    });

    it('adds nothing when the dialog is cancelled', async () => {
      for (const cancelled of [undefined, '']) {
        dialog.answer(cancelled);
        const { el } = await create();
        button(el, 'new-record').click();
        await settle();
      }
      expect(db.insertRow).not.toHaveBeenCalled();
    });

    it('reports a record that cannot be added', async () => {
      dialog.answer({});
      db.insertRow.and.rejectWith(new Error('NOT NULL constraint failed: person.name'));
      const { el } = await create();
      button(el, 'new-record').click();
      await settle();
      expect(notice.error).toHaveBeenCalledWith(jasmine.objectContaining({ message: 'NOT NULL constraint failed: person.name' }));
    });
  });

  describe('the Edit Cell panel', () => {
    const toggle = (el: HTMLElement) => button(el, 'toggle-panel');
    const panelBox = (el: HTMLElement) => query<HTMLTextAreaElement>(el, '.panel-text');

    it('is closed until it is asked for', async () => {
      const { el } = await create();
      expect(el.querySelector('.panel')).toBeNull();
      toggle(el).click();
      await settle();
      expect(el.querySelector('.panel')).not.toBeNull();
      expect(query(el, '.panel .hint').textContent).toContain('Select a cell');
    });

    it('shows the value of the selected cell in full', async () => {
      db.browse.and.callFake(async () => personPage({ rows: [[1, 'a very long text '.repeat(10), 36, null]], keys: [[1]] }));
      const { el } = await create();
      toggle(el).click();
      cell(el, 0, 1).click();
      await settle();
      expect(panelBox(el).value).toBe('a very long text '.repeat(10));
      expect(query(el, '.panel .meta').textContent).toContain('person.name');
      expect(query(el, '.panel .meta').textContent).toContain('170 characters');
    });

    it('follows the selected cell', async () => {
      const { el } = await create();
      toggle(el).click();
      cell(el, 0, 1).click();
      await settle();
      expect(panelBox(el).value).toBe('Ada');
      cell(el, 2, 3).click();
      await settle();
      expect(panelBox(el).value).toBe('third');
    });

    it('writes what is typed when Apply is pressed', async () => {
      const { el } = await create();
      toggle(el).click();
      cell(el, 0, 2).click();
      await settle();
      expect(button(el, 'apply').disabled).toBeTrue();
      typeInto(panelBox(el), '41');
      await settle();
      expect(button(el, 'apply').disabled).toBeFalse();
      button(el, 'apply').click();
      await settle();
      expect(db.updateRow).toHaveBeenCalledOnceWith('person', [1], { age: 41 });
    });

    it('puts the value back on Revert', async () => {
      const { el } = await create();
      toggle(el).click();
      cell(el, 0, 1).click();
      await settle();
      typeInto(panelBox(el), 'Zed');
      await settle();
      button(el, 'reset').click();
      await settle();
      expect(panelBox(el).value).toBe('Ada');
      expect(db.updateRow).not.toHaveBeenCalled();
    });

    it('only shows a cell that cannot be edited', async () => {
      db.browse.and.callFake(async () => personPage({ editable: false, view: true }));
      const { el } = await create();
      toggle(el).click();
      cell(el, 0, 1).click();
      await settle();
      expect(panelBox(el).readOnly).toBeTrue();
      expect(button(el, 'apply').disabled).toBeTrue();
      expect(query(el, '.panel .hint').textContent).toContain('cannot be edited');
    });
  });

  describe('Export CSV', () => {
    function captureDownloads() {
      const blobs: Blob[] = [];
      spyOn(URL, 'createObjectURL').and.callFake(blob => {
        blobs.push(blob as Blob);
        return 'blob:test';
      });
      spyOn(URL, 'revokeObjectURL');
      const names: string[] = [];
      spyOn(HTMLAnchorElement.prototype, 'click').and.callFake(function (this: HTMLAnchorElement) {
        names.push(this.download);
      });
      return { blobs, names };
    }

    it('is off until a table is shown', async () => {
      const { el } = await create(null);
      expect(button(el, 'export').disabled).toBeTrue();
    });

    it('saves every row that matches, in the order of the grid', async () => {
      const { blobs, names } = captureDownloads();
      const { el } = await create();
      el.querySelectorAll<HTMLElement>('th.sortable')[1].click();
      await settle();
      db.browse.calls.reset();
      button(el, 'export').click();
      await new Promise(resolve => setTimeout(resolve, 20));
      await settle();
      expect(db.browse).toHaveBeenCalledOnceWith({ table: 'person', offset: 0, limit: 1000, sort: [{ column: 'name', descending: false }], filters: {} });
      expect(names).toEqual(['person.csv']);
      expect(await blobs[0].text()).toBe('id,name,age,note\r\n1,Ada,36,first\r\n2,Bob,17,\r\n3,Cyd,52,third\r\n');
    });

    it('reads a big table a page at a time', async () => {
      captureDownloads();
      db.browse.and.callFake(async () => personPage({ total: 2500 }));
      const { el } = await create();
      db.browse.calls.reset();
      button(el, 'export').click();
      await new Promise(resolve => setTimeout(resolve, 50));
      await settle();
      expect(db.browse.calls.allArgs().map(args => args[0].offset)).toEqual([0, 1000, 2000]);
    });

    it('reports a table that cannot be read', async () => {
      const { el } = await create();
      db.browse.and.rejectWith(new Error('disk error'));
      button(el, 'export').click();
      await new Promise(resolve => setTimeout(resolve, 20));
      await settle();
      expect(notice.error).toHaveBeenCalledWith(jasmine.objectContaining({ message: 'disk error' }));
    });
  });
});
