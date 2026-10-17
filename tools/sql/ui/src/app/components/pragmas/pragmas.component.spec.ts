import { provideZonelessChangeDetection } from '@angular/core';
import { TestBed } from '@angular/core/testing';

import { FakeDatabase, SAMPLE_PRAGMAS, databaseProviders, fakeDatabase, fakeNotice, query, settle, typeInto } from '../../testing/test-helpers';
import { PragmasComponent } from './pragmas.component';

describe('PragmasComponent', () => {
  let db: FakeDatabase;
  let notice: ReturnType<typeof fakeNotice>;

  beforeEach(() => {
    db = fakeDatabase();
    notice = fakeNotice();
    TestBed.configureTestingModule({ providers: [provideZonelessChangeDetection(), ...databaseProviders(db, notice)] });
  });

  async function create() {
    const fixture = TestBed.createComponent(PragmasComponent);
    await fixture.whenStable();
    return fixture.nativeElement as HTMLElement;
  }

  const pragma = (el: HTMLElement, name: string) => query<HTMLElement>(el, `.pragma[data-name="${name}"]`);
  const save = (el: HTMLElement) => query<HTMLButtonElement>(el, 'button.save');

  it('loads the pragmas once', async () => {
    await create();
    expect(db.pragmas).toHaveBeenCalledTimes(1);
  });

  it('lists every pragma with the control that suits it', async () => {
    const el = await create();
    expect(el.querySelectorAll('.pragma').length).toBe(SAMPLE_PRAGMAS.length);
    expect(pragma(el, 'foreign_keys').querySelector('mat-checkbox')).not.toBeNull();
    expect(pragma(el, 'temp_store').querySelector('mat-select')).not.toBeNull();
    expect(pragma(el, 'user_version').querySelector<HTMLInputElement>('input.number')!.value).toBe('3');
  });

  it('shows a read-only pragma as text', async () => {
    const el = await create();
    expect(pragma(el, 'journal_mode').querySelector('.readonly')!.textContent).toBe('wal');
    expect(pragma(el, 'journal_mode').querySelector('input')).toBeNull();
  });

  it('keeps Save off until a value is changed', async () => {
    const el = await create();
    expect(save(el).disabled).toBeTrue();
    pragma(el, 'foreign_keys').querySelector<HTMLInputElement>('input[type=checkbox]')!.click();
    await settle();
    expect(save(el).disabled).toBeFalse();
  });

  it('turns Save off again when a value is put back', async () => {
    const el = await create();
    const box = pragma(el, 'foreign_keys').querySelector<HTMLInputElement>('input[type=checkbox]')!;
    box.click();
    await settle();
    box.click();
    await settle();
    expect(save(el).disabled).toBeTrue();
  });

  it('saves only what was changed', async () => {
    const el = await create();
    pragma(el, 'foreign_keys').querySelector<HTMLInputElement>('input[type=checkbox]')!.click();
    typeInto(pragma(el, 'user_version').querySelector<HTMLInputElement>('input.number')!, '8');
    await settle();
    save(el).click();
    await settle();
    expect(db.setPragma).toHaveBeenCalledTimes(2);
    expect(db.setPragma).toHaveBeenCalledWith('foreign_keys', 1);
    expect(db.setPragma).toHaveBeenCalledWith('user_version', 8);
    expect(notice.info).toHaveBeenCalledWith('Pragmas saved.');
  });

  it('reports a refused change and shows what the server has', async () => {
    db.setPragma.and.rejectWith(new Error('write or revert the pending changes before changing a PRAGMA'));
    const el = await create();
    typeInto(pragma(el, 'user_version').querySelector<HTMLInputElement>('input.number')!, '8');
    await settle();
    save(el).click();
    await settle();
    expect(notice.error).toHaveBeenCalledWith(jasmine.objectContaining({ message: jasmine.stringContaining('pending changes') }));
    expect(db.pragmas).toHaveBeenCalledTimes(2);
    expect(pragma(el, 'user_version').querySelector<HTMLInputElement>('input.number')!.value).toBe('3');
  });

  it('refreshes on demand and when the database changes', async () => {
    const el = await create();
    query<HTMLButtonElement>(el, 'button.refresh').click();
    await settle();
    expect(db.pragmas).toHaveBeenCalledTimes(2);
    db.revision.update(count => count + 1);
    await settle();
    expect(db.pragmas).toHaveBeenCalledTimes(3);
  });

  it('reports pragmas that cannot be read', async () => {
    db.pragmas.and.rejectWith(new Error('The server cannot be reached.'));
    await create();
    expect(notice.error).toHaveBeenCalledTimes(1);
  });
});
