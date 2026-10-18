import { provideZonelessChangeDetection } from '@angular/core';
import { EditorView } from '@codemirror/view';
import { TestBed } from '@angular/core/testing';

import { FakeDatabase, databaseProviders, fakeDatabase, fakeNotice, query, settle, sqlReply } from '../../testing/test-helpers';
import { ExecuteSqlComponent, describeRun } from './execute-sql.component';

describe('describeRun', () => {
  it('counts the rows of a query', () => {
    expect(describeRun(sqlReply({ columns: ['a'], rows: [[1]], rowCount: 1, elapsedMs: 2.6 }))).toBe('1 row returned in 3ms');
    expect(describeRun(sqlReply({ columns: ['a'], rows: [], rowCount: 0, elapsedMs: 0.2 }))).toBe('0 rows returned in 0ms');
  });

  it('says how many rows were not shown', () => {
    expect(describeRun(sqlReply({ columns: ['a'], rows: [[1], [2]], rowCount: 9000, truncated: true, elapsedMs: 10 }))).toBe(
      '9000 rows returned in 10ms, the first 2 are shown',
    );
  });

  it('counts the rows a statement changed', () => {
    expect(describeRun(sqlReply({ changes: 3, elapsedMs: 1 }))).toBe('Query executed successfully, 3 rows affected. Took 1ms');
    expect(describeRun(sqlReply({ changes: 1, elapsedMs: 1 }))).toBe('Query executed successfully, 1 row affected. Took 1ms');
  });
});

describe('ExecuteSqlComponent', () => {
  let db: FakeDatabase;

  beforeEach(() => {
    db = fakeDatabase();
    TestBed.configureTestingModule({ providers: [provideZonelessChangeDetection(), ...databaseProviders(db, fakeNotice())] });
  });

  async function create() {
    const fixture = TestBed.createComponent(ExecuteSqlComponent);
    await fixture.whenStable();
    return fixture.nativeElement as HTMLElement;
  }

  const view = (el: HTMLElement) => EditorView.findFromDOM(query(el, '.cm-editor'))!;
  const text = (el: HTMLElement) => view(el).state.doc.toString();
  const setSql = async (el: HTMLElement, sql: string) => {
    view(el).dispatch({ changes: { from: 0, to: view(el).state.doc.length, insert: sql } });
    await settle();
  };
  const run = async (el: HTMLElement) => {
    query<HTMLButtonElement>(el, 'button.run').click();
    await settle();
  };
  const message = (el: HTMLElement) => query(el, 'pre.message').textContent;
  const tabTitles = (el: HTMLElement) => Array.from(el.querySelectorAll('.tab-title')).map(title => title.textContent);

  it('starts with one empty editor', async () => {
    const el = await create();
    expect(tabTitles(el)).toEqual(['SQL 1']);
    expect(text(el)).toBe('');
    expect(message(el)).toBe('');
  });

  it('runs the text of the editor and shows the rows', async () => {
    db.execute.and.callFake(async () => sqlReply({ columns: ['id', 'name'], rows: [[1, 'Ada'], [2, null]], rowCount: 2 }));
    const el = await create();
    await setSql(el, 'SELECT id, name FROM person');
    await run(el);
    expect(db.execute).toHaveBeenCalledOnceWith('SELECT id, name FROM person');
    expect(Array.from(el.querySelectorAll('.results th.grid-header .title')).map(th => th.textContent!.trim())).toEqual(['id', 'name']);
    const rows = Array.from(el.querySelectorAll('.results tr.grid-row')).map(row =>
      Array.from(row.querySelectorAll('td:not(.grid-filler)')).map(td => td.textContent!.trim()),
    );
    expect(rows).toEqual([
      ['1', '1', 'Ada'],
      ['2', '2', 'NULL'],
    ]);
    expect(message(el)).toBe('2 rows returned in 1ms');
  });

  it('shows two result columns of one name apart', async () => {
    db.execute.and.callFake(async () => sqlReply({ columns: ['a', 'a'], rows: [[1, 2]], rowCount: 1 }));
    const el = await create();
    await setSql(el, 'SELECT 1 AS a, 2 AS a');
    await run(el);
    expect(Array.from(el.querySelectorAll('.results tr.grid-row td:not(.grid-filler)')).map(td => td.textContent!.trim())).toEqual(['1', '1', '2']);
  });

  it('sorts the rows of a result in the grid the way SQLite orders values', async () => {
    db.execute.and.callFake(async () =>
      sqlReply({ columns: ['kind', 'n'], rows: [['b', 10], ['a', 'text'], ['b', null], ['a', 2]], rowCount: 4 }),
    );
    const el = await create();
    await setSql(el, 'SELECT kind, n FROM t');
    await run(el);
    el.querySelectorAll<HTMLElement>('.results th.grid-header')[1].click();
    await settle();
    const values = () => Array.from(el.querySelectorAll('.results tr.grid-row')).map(row => row.querySelectorAll('td')[2].textContent!.trim());
    expect(values()).toEqual(['NULL', '2', '10', 'text']);
    expect(db.execute).toHaveBeenCalledTimes(1);
  });

  it('runs on Ctrl+Enter', async () => {
    const el = await create();
    await setSql(el, 'SELECT 1');
    query(el, '.cm-content').dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', ctrlKey: true, bubbles: true, cancelable: true }));
    await settle();
    expect(db.execute).toHaveBeenCalledOnceWith('SELECT 1');
  });

  it('runs only the selection when there is one', async () => {
    const el = await create();
    await setSql(el, 'SELECT 1; SELECT 2;');
    view(el).dispatch({ selection: { anchor: 10, head: 19 } });
    await run(el);
    expect(db.execute).toHaveBeenCalledOnceWith('SELECT 2;');
  });

  it('runs nothing for an empty editor', async () => {
    const el = await create();
    await setSql(el, '  \n ');
    await run(el);
    expect(db.execute).not.toHaveBeenCalled();
  });

  it('reports what a statement changed and shows no table', async () => {
    db.execute.and.callFake(async () => sqlReply({ changes: 4, inTransaction: true }));
    const el = await create();
    await setSql(el, 'UPDATE person SET age = 1');
    await run(el);
    expect(message(el)).toBe('Query executed successfully, 4 rows affected. Took 1ms');
    expect(el.querySelector('.results table')).toBeNull();
    expect(query(el, 'pre.message').classList.contains('failed')).toBeFalse();
  });

  it('reports an error and drops the old result', async () => {
    db.execute.and.callFake(async () => sqlReply({ columns: ['a'], rows: [[1]], rowCount: 1 }));
    const el = await create();
    await setSql(el, 'SELECT 1');
    await run(el);
    db.execute.and.rejectWith(new Error('no such table: nope'));
    await setSql(el, 'SELECT * FROM nope');
    await run(el);
    expect(message(el)).toBe('Execution aborted: no such table: nope');
    expect(query(el, 'pre.message').classList.contains('failed')).toBeTrue();
    expect(el.querySelector('.results table')).toBeNull();
  });

  it('shows a blob and a NULL as labels', async () => {
    db.execute.and.callFake(async () => sqlReply({ columns: ['a', 'b'], rows: [[null, { type: 'blob', size: 3, value: '000000' }]], rowCount: 1 }));
    const el = await create();
    await setSql(el, 'SELECT 1');
    await run(el);
    expect(Array.from(el.querySelectorAll('.results .placeholder')).map(label => label.textContent)).toEqual(['NULL', 'BLOB (3 bytes)']);
  });

  it('does not start a second run while one is going', async () => {
    let finish: (reply: ReturnType<typeof sqlReply>) => void = () => undefined;
    db.execute.and.callFake(() => new Promise(resolve => (finish = resolve)));
    const el = await create();
    await setSql(el, 'SELECT 1');
    await run(el);
    expect(query<HTMLButtonElement>(el, 'button.run').disabled).toBeTrue();
    await run(el);
    expect(db.execute).toHaveBeenCalledTimes(1);
    finish(sqlReply());
    await new Promise(resolve => setTimeout(resolve));
    await settle();
    expect(query<HTMLButtonElement>(el, 'button.run').disabled).toBeFalse();
  });

  describe('tabs', () => {
    it('opens a new editor that does not share the text', async () => {
      const el = await create();
      await setSql(el, 'SELECT 1');
      query<HTMLButtonElement>(el, 'button.add-tab').click();
      await settle();
      expect(tabTitles(el)).toEqual(['SQL 1', 'SQL 2']);
      expect(text(el)).toBe('');
      await setSql(el, 'SELECT 2');
      await run(el);
      expect(db.execute).toHaveBeenCalledOnceWith('SELECT 2');
    });

    it('closes an editor, and replaces the last one with an empty one', async () => {
      const el = await create();
      query<HTMLButtonElement>(el, 'button.add-tab').click();
      await settle();
      query<HTMLButtonElement>(el, 'button.close-tab').click();
      await settle();
      expect(tabTitles(el)).toEqual(['SQL 2']);
      query<HTMLButtonElement>(el, 'button.close-tab').click();
      await settle();
      expect(tabTitles(el)).toEqual(['SQL 3']);
    });
  });

  describe('Save as CSV', () => {
    it('is off until there are rows', async () => {
      const el = await create();
      expect(query<HTMLButtonElement>(el, 'button.export').disabled).toBeTrue();
    });

    it('hands the rows to the browser as a file', async () => {
      db.execute.and.callFake(async () => sqlReply({ columns: ['id', 'name'], rows: [[1, 'a,b']], rowCount: 1 }));
      const blobs: Blob[] = [];
      spyOn(URL, 'createObjectURL').and.callFake(blob => {
        blobs.push(blob as Blob);
        return 'blob:test';
      });
      spyOn(URL, 'revokeObjectURL');
      const downloads: string[] = [];
      spyOn(HTMLAnchorElement.prototype, 'click').and.callFake(function (this: HTMLAnchorElement) {
        downloads.push(this.download);
      });

      const el = await create();
      await setSql(el, 'SELECT 1');
      await run(el);
      query<HTMLButtonElement>(el, 'button.export').click();
      await settle();
      expect(downloads).toEqual(['sql-1.csv']);
      expect(await blobs[0].text()).toBe('id,name\r\n1,"a,b"\r\n');
    });
  });

  describe('SQL files', () => {
    it('saves the text of the tab under the name of the tab', async () => {
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
      const el = await create();
      expect(query<HTMLButtonElement>(el, 'button.save-file').disabled).toBeTrue();
      await setSql(el, 'SELECT 1;');
      query<HTMLButtonElement>(el, 'button.save-file').click();
      await settle();
      expect(names).toEqual(['sql-1.sql']);
      expect(await blobs[0].text()).toBe('SELECT 1;');
    });

    it('loads a file into the tab and names the tab after it', async () => {
      const el = await create();
      const input = query<HTMLInputElement>(el, 'input[type=file]');
      const files = new DataTransfer();
      files.items.add(new File(['SELECT 42;'], 'report.sql', { type: 'text/plain' }));
      input.files = files.files;
      input.dispatchEvent(new Event('change'));
      await new Promise(resolve => setTimeout(resolve, 20));
      await settle();
      expect(text(el)).toBe('SELECT 42;');
      expect(tabTitles(el)).toEqual(['report.sql']);
    });
  });
});
