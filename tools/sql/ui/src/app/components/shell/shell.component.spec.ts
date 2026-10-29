import { provideZonelessChangeDetection, signal } from '@angular/core';
import { TestBed } from '@angular/core/testing';

import { FakeDatabase, databaseProviders, fakeDatabase, fakeInfo, fakeNotice, query, settle } from '../../testing/test-helpers';
import { ReportService } from '../../reports/report.service';
import { ShellComponent } from './shell.component';

describe('ShellComponent', () => {
  let db: FakeDatabase;
  let notice: ReturnType<typeof fakeNotice>;
  /** Whether the server has the report features (a build without SQL_WITH_REPORTS has not). */
  const reportsAvailable = signal<boolean | null>(true);

  beforeEach(() => {
    reportsAvailable.set(true);
    db = fakeDatabase();
    notice = fakeNotice();
    const reports = { available: reportsAvailable, probe: jasmine.createSpy('probe').and.resolveTo(true) };
    TestBed.configureTestingModule({ providers: [provideZonelessChangeDetection(), ...databaseProviders(db, notice), { provide: ReportService, useValue: reports }] });
  });

  async function create() {
    const fixture = TestBed.createComponent(ShellComponent);
    await fixture.whenStable();
    return { fixture, el: fixture.nativeElement as HTMLElement };
  }

  it('loads the database when it starts', async () => {
    await create();
    expect(db.load).toHaveBeenCalledTimes(1);
  });

  it('shows where the database is and which version serves it', async () => {
    const { el } = await create();
    expect(query(el, '.path').textContent).toContain('/data/people.sqldb');
    expect(query(el, '.status').textContent).toContain('libsql 0.2.0');
  });

  const tabs = (el: HTMLElement): (string | undefined)[] => Array.from(el.querySelectorAll('.mdc-tab')).map(tab => tab.textContent?.trim());

  it('has the four tabs of DB Browser, Visualize and the two report tabs', async () => {
    const { el } = await create();
    expect(tabs(el)).toEqual(['Database Structure', 'Browse Data', 'Edit Pragmas', 'Execute SQL', 'Visualize', 'Reports', 'Report Designer']);
  });

  it('hides the report tabs when the server has no reports, and the designer when the database is read-only', async () => {
    reportsAvailable.set(false);
    const { el, fixture } = await create();
    expect(tabs(el)).toEqual(['Database Structure', 'Browse Data', 'Edit Pragmas', 'Execute SQL', 'Visualize']);

    reportsAvailable.set(true);
    db.info.set({ ...fakeInfo(), readOnly: true });
    await fixture.whenStable();
    await settle();
    expect(tabs(el)).toEqual(['Database Structure', 'Browse Data', 'Edit Pragmas', 'Execute SQL', 'Visualize', 'Reports']);
  });

  it('keeps Write Changes and Revert Changes off until something is pending', async () => {
    const { el } = await create();
    expect(query<HTMLButtonElement>(el, 'button.write').disabled).toBeTrue();
    expect(query<HTMLButtonElement>(el, 'button.revert').disabled).toBeTrue();
    expect(el.querySelector('.pending')).toBeNull();

    db.inTransaction.set(true);
    await settle();
    expect(query<HTMLButtonElement>(el, 'button.write').disabled).toBeFalse();
    expect(query<HTMLButtonElement>(el, 'button.revert').disabled).toBeFalse();
    expect(query(el, '.pending').textContent).toContain('waiting');
  });

  it('writes the changes', async () => {
    const { el } = await create();
    db.inTransaction.set(true);
    await settle();
    query<HTMLButtonElement>(el, 'button.write').click();
    await settle();
    expect(db.commit).toHaveBeenCalledTimes(1);
    expect(notice.info).toHaveBeenCalledWith('Changes written.');
  });

  it('reverts the changes', async () => {
    const { el } = await create();
    db.inTransaction.set(true);
    await settle();
    query<HTMLButtonElement>(el, 'button.revert').click();
    await settle();
    expect(db.revert).toHaveBeenCalledTimes(1);
    expect(notice.info).toHaveBeenCalledWith('Changes reverted.');
  });

  it('writes on Ctrl+S, but only when there is something to write', async () => {
    await create();
    const press = () => document.dispatchEvent(new KeyboardEvent('keydown', { key: 's', ctrlKey: true, cancelable: true }));
    press();
    await settle();
    expect(db.commit).not.toHaveBeenCalled();

    db.inTransaction.set(true);
    await settle();
    press();
    await settle();
    expect(db.commit).toHaveBeenCalledTimes(1);
  });

  it('reports a failed write', async () => {
    db.commit.and.rejectWith(new Error('disk full'));
    const { el } = await create();
    db.inTransaction.set(true);
    await settle();
    query<HTMLButtonElement>(el, 'button.write').click();
    await settle();
    expect(notice.error).toHaveBeenCalledWith(jasmine.objectContaining({ message: 'disk full' }));
  });

  it('reports a database that cannot be loaded', async () => {
    db.load.and.rejectWith(new Error('The server cannot be reached.'));
    await create();
    expect(notice.error).toHaveBeenCalledTimes(1);
  });

  it('says when the database is read-only', async () => {
    db.info.set(fakeInfo({ readOnly: true }));
    const { el } = await create();
    expect(query(el, '.status').textContent).toContain('Read-only');
  });

  it('says whether the database can be written', async () => {
    const { el } = await create();
    expect(query(el, '.mode').textContent).toBe('read-write');
    db.info.set(fakeInfo({ readOnly: true }));
    await settle();
    expect(query(el, '.mode').textContent).toBe('read-only');
    expect(query(el, '.mode').classList.contains('read-only')).toBeTrue();
  });

  it('asks before the page is closed with changes waiting', async () => {
    const { fixture } = await create();
    // Karma treats a real beforeunload as a failed run, so the handler is called directly.
    const close = () => {
      const event = jasmine.createSpyObj<BeforeUnloadEvent>('event', ['preventDefault']);
      fixture.componentInstance['onUnload'](event);
      return event.preventDefault.calls.count() > 0;
    };
    expect(close()).toBeFalse();
    db.inTransaction.set(true);
    expect(close()).toBeTrue();
  });
});
