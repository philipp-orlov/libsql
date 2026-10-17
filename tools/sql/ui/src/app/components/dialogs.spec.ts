import { provideZonelessChangeDetection, Type } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { OverlayContainer } from '@angular/cdk/overlay';
import { MatDialog, MatDialogRef } from '@angular/material/dialog';
import { firstValueFrom } from 'rxjs';

import { PERSON_COLUMNS, query, sampleSchema, settle, typeInto } from '../testing/test-helpers';
import { ConfirmDialogComponent } from './confirm-dialog/confirm-dialog.component';
import { CreateIndexDialogComponent } from './create-index-dialog/create-index-dialog.component';
import { CreateTableDialogComponent } from './create-table-dialog/create-table-dialog.component';
import { ModifyTableDialogComponent } from './modify-table-dialog/modify-table-dialog.component';
import { RecordDialogComponent } from './record-dialog/record-dialog.component';

describe('dialogs', () => {
  beforeEach(() => TestBed.configureTestingModule({ providers: [provideZonelessChangeDetection()] }));
  afterEach(() => TestBed.inject(MatDialog).closeAll());

  async function open<T>(component: Type<T>, data?: unknown) {
    const ref: MatDialogRef<T> = TestBed.inject(MatDialog).open(component, { data });
    await settle();
    const el = TestBed.inject(OverlayContainer).getContainerElement();
    return { ref, el, closed: firstValueFrom(ref.afterClosed()) };
  }

  const click = async (el: HTMLElement, selector: string) => {
    query<HTMLElement>(el, selector).click();
    await settle();
  };
  const preview = (el: HTMLElement) => query(el, 'pre.preview').textContent;
  const ok = (el: HTMLElement) => query<HTMLButtonElement>(el, 'button.ok');

  describe('ConfirmDialogComponent', () => {
    const data = { title: 'Delete table', message: 'Are you sure?', action: 'Delete' };

    it('shows the question and answers true on confirmation', async () => {
      const { el, closed } = await open(ConfirmDialogComponent, data);
      expect(el.textContent).toContain('Delete table');
      expect(el.textContent).toContain('Are you sure?');
      await click(el, 'button.confirm');
      expect(await closed).toBeTrue();
    });

    it('answers nothing on Cancel', async () => {
      const { el, closed } = await open(ConfirmDialogComponent, data);
      const cancel = Array.from(el.querySelectorAll('button')).find(b => b.textContent!.trim() === 'Cancel')!;
      cancel.click();
      expect(await closed).toBeFalsy();
    });
  });

  describe('CreateTableDialogComponent', () => {
    it('starts with an id column and writes it', async () => {
      const { el } = await open(CreateTableDialogComponent);
      expect(preview(el)).toBe('CREATE TABLE "" (\n\t"id" INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL\n);');
    });

    it('needs a table name', async () => {
      const { el } = await open(CreateTableDialogComponent);
      expect(ok(el).disabled).toBeTrue();
      typeInto(query<HTMLInputElement>(el, 'input[matinput]'), 'notes');
      await settle();
      expect(ok(el).disabled).toBeFalse();
      expect(preview(el)).toContain('CREATE TABLE "notes"');
    });

    it('adds and removes columns', async () => {
      const { el } = await open(CreateTableDialogComponent);
      typeInto(query<HTMLInputElement>(el, 'input[matinput]'), 'notes');
      await click(el, 'button.add');
      expect(el.querySelectorAll('tbody tr').length).toBe(2);
      // The new column has no name yet.
      expect(ok(el).disabled).toBeTrue();
      typeInto(el.querySelectorAll<HTMLInputElement>('input.column-name')[1], 'body');
      await settle();
      expect(ok(el).disabled).toBeFalse();
      expect(preview(el)).toContain('\t"body" TEXT');

      await click(el, 'button.remove');
      expect(el.querySelectorAll('tbody tr').length).toBe(1);
      expect(preview(el)).not.toContain('"id"');
    });

    it('writes the column flags', async () => {
      const { el } = await open(CreateTableDialogComponent);
      typeInto(query<HTMLInputElement>(el, 'input[matinput]'), 'notes');
      const boxes = el.querySelectorAll<HTMLInputElement>('tbody input[type=checkbox]');
      // The id column starts with NN, PK and AI on; switch AI and NN off and Unique on.
      boxes[2].click();
      boxes[0].click();
      boxes[3].click();
      await settle();
      expect(preview(el)).toBe('CREATE TABLE "notes" (\n\t"id" INTEGER UNIQUE,\n\tPRIMARY KEY("id")\n);');
    });

    it('closes with the statement', async () => {
      const { el, closed } = await open(CreateTableDialogComponent);
      typeInto(query<HTMLInputElement>(el, 'input[matinput]'), 'notes');
      await settle();
      ok(el).click();
      expect(await closed).toBe('CREATE TABLE "notes" (\n\t"id" INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL\n);');
    });
  });

  describe('CreateIndexDialogComponent', () => {
    const data = { tables: sampleSchema().tables, table: null };

    it('starts on the first table with no column chosen', async () => {
      const { el } = await open(CreateIndexDialogComponent, data);
      expect(el.querySelectorAll('.column').length).toBe(4);
      expect(ok(el).disabled).toBeTrue();
    });

    it('starts on the table it is told to', async () => {
      const { el } = await open(CreateIndexDialogComponent, { ...data, table: 'tag' });
      expect(Array.from(el.querySelectorAll('.column .use')).map(c => c.textContent!.trim())).toEqual(['label']);
    });

    it('writes the index and closes with it', async () => {
      const { el, closed } = await open(CreateIndexDialogComponent, data);
      typeInto(query<HTMLInputElement>(el, 'input[matinput]'), 'by_age');
      const uses = el.querySelectorAll<HTMLInputElement>('.column .use input');
      uses[2].click();
      await settle();
      el.querySelectorAll<HTMLInputElement>('.column .desc input')[2].click();
      await settle();
      expect(preview(el)).toBe('CREATE INDEX "by_age" ON "person" ("age" DESC);');
      el.querySelectorAll<HTMLInputElement>('.column .use input')[1].click();
      await settle();
      expect(preview(el)).toBe('CREATE INDEX "by_age" ON "person" ("name", "age" DESC);');
      ok(el).click();
      expect(await closed).toBe('CREATE INDEX "by_age" ON "person" ("name", "age" DESC);');
    });

    it('can only turn Descending on for a column that is used', async () => {
      const { el } = await open(CreateIndexDialogComponent, data);
      expect(el.querySelector<HTMLInputElement>('.column .desc input')!.disabled).toBeTrue();
    });
  });

  describe('ModifyTableDialogComponent', () => {
    const table = sampleSchema().tables[0];

    it('has nothing to do until something is changed', async () => {
      const { el } = await open(ModifyTableDialogComponent, table);
      expect(el.querySelectorAll('tbody tr').length).toBe(4);
      expect(ok(el).disabled).toBeTrue();
      expect(preview(el)).toBe('');
    });

    it('renames the table, and does it last', async () => {
      const { el, closed } = await open(ModifyTableDialogComponent, table);
      typeInto(query<HTMLInputElement>(el, 'input.table-name'), 'people');
      typeInto(el.querySelectorAll<HTMLInputElement>('input.existing-name')[3], 'remark');
      await settle();
      expect(preview(el)).toBe('ALTER TABLE "person" RENAME COLUMN "note" TO "remark";\nALTER TABLE "person" RENAME TO "people";');
      ok(el).click();
      expect(await closed).toBe('ALTER TABLE "person" RENAME COLUMN "note" TO "remark";\nALTER TABLE "person" RENAME TO "people";');
    });

    it('drops a column, and takes it back', async () => {
      const { el } = await open(ModifyTableDialogComponent, table);
      await click(el, 'tbody tr:nth-child(3) button.drop');
      expect(preview(el)).toBe('ALTER TABLE "person" DROP COLUMN "age";');
      await click(el, 'tbody tr:nth-child(3) button.drop');
      expect(preview(el)).toBe('');
    });

    it('does not rename a column that is dropped', async () => {
      const { el } = await open(ModifyTableDialogComponent, table);
      typeInto(el.querySelectorAll<HTMLInputElement>('input.existing-name')[2], 'years');
      await settle();
      await click(el, 'tbody tr:nth-child(3) button.drop');
      expect(preview(el)).toBe('ALTER TABLE "person" DROP COLUMN "age";');
    });

    it('adds a column once it has a name', async () => {
      const { el } = await open(ModifyTableDialogComponent, table);
      await click(el, 'button.add');
      expect(ok(el).disabled).toBeTrue();
      typeInto(query<HTMLInputElement>(el, 'input.added-name'), 'email');
      await settle();
      expect(preview(el)).toBe('ALTER TABLE "person" ADD COLUMN "email" TEXT;');
      expect(ok(el).disabled).toBeFalse();

      await click(el, 'button.remove-added');
      expect(preview(el)).toBe('');
    });
  });

  describe('RecordDialogComponent', () => {
    const data = {
      table: 'person',
      columns: [...PERSON_COLUMNS, { ...PERSON_COLUMNS[2], name: 'double', generated: 1 }, { ...PERSON_COLUMNS[3], name: 'tag', defaultSql: "'new'" }],
    };

    it('asks for every column that can be written, with a hint for what an empty box means', async () => {
      const { el } = await open(RecordDialogComponent, data);
      const hints = Array.from(el.querySelectorAll<HTMLInputElement>('input.value')).map(input => input.placeholder);
      expect(hints).toEqual(['assigned automatically', 'required', 'NULL', 'NULL', "default 'new'"]);
    });

    it('closes with the values that were filled in, typed by their column', async () => {
      const { el, closed } = await open(RecordDialogComponent, data);
      const boxes = el.querySelectorAll<HTMLInputElement>('input.value');
      typeInto(boxes[1], 'Eve');
      typeInto(boxes[2], '29');
      await settle();
      query<HTMLButtonElement>(el, 'button.ok').click();
      expect(await closed).toEqual({ name: 'Eve', age: 29 });
    });

    it('closes with nothing on Cancel', async () => {
      const { el, closed } = await open(RecordDialogComponent, data);
      Array.from(el.querySelectorAll('button')).find(b => b.textContent!.trim() === 'Cancel')!.click();
      expect(await closed).toBeFalsy();
    });

    it('closes with nothing for a record left empty', async () => {
      const { el, closed } = await open(RecordDialogComponent, data);
      query<HTMLButtonElement>(el, 'button.ok').click();
      expect(await closed).toEqual({});
    });
  });
});
