import { provideZonelessChangeDetection } from '@angular/core';
import { TestBed } from '@angular/core/testing';

import { WorkspaceService } from '../../services/workspace.service';
import { FakeDatabase, databaseProviders, dialogAnswering, fakeDatabase, fakeInfo, fakeNotice, query, settle } from '../../testing/test-helpers';
import { ConfirmDialogComponent } from '../confirm-dialog/confirm-dialog.component';
import { CreateIndexDialogComponent } from '../create-index-dialog/create-index-dialog.component';
import { CreateTableDialogComponent } from '../create-table-dialog/create-table-dialog.component';
import { ModifyTableDialogComponent } from '../modify-table-dialog/modify-table-dialog.component';
import { StructureComponent } from './structure.component';

describe('StructureComponent', () => {
  let db: FakeDatabase;
  let notice: ReturnType<typeof fakeNotice>;
  let dialog: ReturnType<typeof dialogAnswering>;

  beforeEach(() => {
    db = fakeDatabase();
    notice = fakeNotice();
    dialog = dialogAnswering(undefined);
    TestBed.configureTestingModule({ providers: [provideZonelessChangeDetection(), ...databaseProviders(db, notice), dialog.provider] });
  });

  async function create() {
    const fixture = TestBed.createComponent(StructureComponent);
    await fixture.whenStable();
    return fixture.nativeElement as HTMLElement;
  }

  const names = (el: HTMLElement) => Array.from(el.querySelectorAll('.tree .row[data-kind]')).map(row => row.querySelector('.label')!.textContent!.trim());
  const row = (el: HTMLElement, name: string) =>
    Array.from(el.querySelectorAll<HTMLElement>('.tree .row[data-kind]')).find(candidate => candidate.querySelector('.label')!.textContent!.trim() === name)!;
  const button = (el: HTMLElement, name: string) => query<HTMLButtonElement>(el, `button.${name}`);

  it('lists the tables first and the other objects under their groups, with counts', async () => {
    const el = await create();
    expect(names(el)).toEqual(['Tables (2)', 'person', 'tag', 'Indices (1)', 'Views (1)', 'Triggers (1)']);
  });

  it('shows the definition of an object on its row', async () => {
    const el = await create();
    expect(row(el, 'person').querySelector('.schema')!.textContent).toBe('CREATE TABLE person( id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER, note TEXT)');
  });

  it('opens and closes a group', async () => {
    const el = await create();
    query<HTMLButtonElement>(row(el, 'Indices (1)'), 'button.toggle').click();
    await settle();
    expect(names(el)).toContain('person_name');
    query<HTMLButtonElement>(row(el, 'Tables (2)'), 'button.toggle').click();
    await settle();
    expect(names(el)).toEqual(['Tables (2)', 'Indices (1)', 'person_name', 'Views (1)', 'Triggers (1)']);
  });

  it('shows the columns of a table with their types and definitions', async () => {
    const el = await create();
    query<HTMLButtonElement>(row(el, 'person'), 'button.toggle').click();
    await settle();
    const columns = Array.from(el.querySelectorAll('.row[data-kind="column"]'));
    expect(columns.map(c => c.querySelector('.label')!.textContent!.trim())).toEqual(['id', 'name', 'age', 'note']);
    expect(columns[1].querySelector('.type')!.textContent).toBe('TEXT');
    expect(columns[1].querySelector('.schema')!.textContent).toBe('"name" TEXT NOT NULL');
    expect(columns[0].querySelector('.schema')!.textContent).toBe('"id" INTEGER PRIMARY KEY');
  });

  it('opens a table in Browse Data on a double click', async () => {
    const el = await create();
    row(el, 'tag').dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
    await settle();
    const workspace = TestBed.inject(WorkspaceService);
    expect(workspace.table()).toBe('tag');
    expect(workspace.tab()).toBe(1);
  });

  it('can modify or delete only what is selected', async () => {
    const el = await create();
    expect(button(el, 'modify-table').disabled).toBeTrue();
    expect(button(el, 'delete').disabled).toBeTrue();

    row(el, 'person').click();
    await settle();
    expect(button(el, 'modify-table').disabled).toBeFalse();
    expect(button(el, 'delete').disabled).toBeFalse();

    row(el, 'Views (1)').click();
    await settle();
    expect(button(el, 'modify-table').disabled).toBeTrue();
    expect(button(el, 'delete').disabled).toBeTrue();
  });

  it('runs the statement a Create Table dialog returns', async () => {
    dialog.answer('CREATE TABLE "t" ("a");');
    const el = await create();
    button(el, 'create-table').click();
    await settle();
    expect(dialog.open).toHaveBeenCalledWith(CreateTableDialogComponent, jasmine.anything());
    expect(db.execute).toHaveBeenCalledWith('CREATE TABLE "t" ("a");');
  });

  it('runs nothing when the dialog is cancelled', async () => {
    const el = await create();
    button(el, 'create-table').click();
    await settle();
    expect(db.execute).not.toHaveBeenCalled();
  });

  it('offers the tables, and the selected one first, to Create Index', async () => {
    dialog.answer('CREATE INDEX "i" ON "tag" ("label");');
    const el = await create();
    row(el, 'tag').click();
    await settle();
    button(el, 'create-index').click();
    await settle();
    const [component, config] = dialog.open.calls.mostRecent().args;
    expect(component).toBe(CreateIndexDialogComponent);
    expect(config.data.table).toBe('tag');
    expect(config.data.tables.map((t: { name: string }) => t.name)).toEqual(['person', 'tag']);
    expect(db.execute).toHaveBeenCalledWith('CREATE INDEX "i" ON "tag" ("label");');
  });

  it('hands the selected table to Modify Table, also when a column is selected', async () => {
    dialog.answer('ALTER TABLE "person" RENAME TO "people";');
    const el = await create();
    query<HTMLButtonElement>(row(el, 'person'), 'button.toggle').click();
    await settle();
    row(el, 'age').click();
    await settle();
    button(el, 'modify-table').click();
    await settle();
    const [component, config] = dialog.open.calls.mostRecent().args;
    expect(component).toBe(ModifyTableDialogComponent);
    expect(config.data.name).toBe('person');
    expect(db.execute).toHaveBeenCalledWith('ALTER TABLE "person" RENAME TO "people";');
  });

  it('deletes an object after a confirmation', async () => {
    dialog.answer(true);
    const el = await create();
    query<HTMLButtonElement>(row(el, 'Indices (1)'), 'button.toggle').click();
    await settle();
    row(el, 'person_name').click();
    await settle();
    button(el, 'delete').click();
    await settle();
    expect(dialog.open.calls.mostRecent().args[0]).toBe(ConfirmDialogComponent);
    expect(db.execute).toHaveBeenCalledWith('DROP INDEX "person_name";');
  });

  it('deletes nothing when the confirmation is refused', async () => {
    const el = await create();
    row(el, 'tag').click();
    await settle();
    button(el, 'delete').click();
    await settle();
    expect(db.execute).not.toHaveBeenCalled();
  });

  it('reports a statement that fails', async () => {
    db.execute.and.rejectWith(new Error('table exists'));
    dialog.answer('CREATE TABLE person (a);');
    const el = await create();
    button(el, 'create-table').click();
    await settle();
    expect(notice.error).toHaveBeenCalledWith(jasmine.objectContaining({ message: 'table exists' }));
  });

  it('does not offer to change a read-only database', async () => {
    db.info.set(fakeInfo({ readOnly: true }));
    const el = await create();
    row(el, 'person').click();
    await settle();
    expect(button(el, 'create-table').disabled).toBeTrue();
    expect(button(el, 'create-index').disabled).toBeTrue();
    expect(button(el, 'modify-table').disabled).toBeTrue();
    expect(button(el, 'delete').disabled).toBeTrue();
  });

  it('follows the schema when it changes', async () => {
    const el = await create();
    db.schema.update(schema => ({ ...schema, tables: schema.tables.filter(table => table.name !== 'tag') }));
    await settle();
    expect(names(el)).toEqual(['Tables (1)', 'person', 'Indices (1)', 'Views (1)', 'Triggers (1)']);
  });

  describe('Export SQL', () => {
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

    it('downloads the script under the name of the database', async () => {
      const { blobs, names } = captureDownloads();
      const el = await create();
      button(el, 'export-sql').click();
      await settle();
      expect(db.dump).toHaveBeenCalledTimes(1);
      expect(names).toEqual(['people.sql']);
      expect(await blobs[0].text()).toBe('BEGIN TRANSACTION;\nCOMMIT;\n');
    });

    it('is open to a read-only database', async () => {
      db.info.set(fakeInfo({ readOnly: true }));
      const el = await create();
      expect(button(el, 'export-sql').disabled).toBeFalse();
    });

    it('reports a script that cannot be made', async () => {
      db.dump.and.rejectWith(new Error('out of memory'));
      const el = await create();
      button(el, 'export-sql').click();
      await settle();
      expect(notice.error).toHaveBeenCalledWith(jasmine.objectContaining({ message: 'out of memory' }));
    });
  });
});
