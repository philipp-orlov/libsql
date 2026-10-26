import { ApplicationRef, signal } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { of } from 'rxjs';

import { MatDialog } from '@angular/material/dialog';

import { BrowseReply, ColumnInfo, DatabaseInfo, PragmaInfo, Schema, SqlReply, SqlResult } from '../models/api.models';
import { fromReply } from '../utils/sql-result';
import { DatabaseService } from '../services/database.service';
import { NoticeService } from '../services/notice.service';

export function column(name: string, type = 'TEXT', overrides: Partial<ColumnInfo> = {}): ColumnInfo {
  return {
    name,
    type,
    affinity: type === 'INTEGER' ? 'INTEGER' : type === 'REAL' ? 'REAL' : 'TEXT',
    primaryKey: false,
    notNull: false,
    unique: false,
    generated: 0,
    defaultSql: null,
    ...overrides,
  };
}

export const PERSON_COLUMNS: ColumnInfo[] = [
  column('id', 'INTEGER', { primaryKey: true }),
  column('name', 'TEXT', { notNull: true }),
  column('age', 'INTEGER'),
  column('note', 'TEXT'),
];

export function sampleSchema(): Schema {
  return {
    tables: [
      { name: 'person', sql: 'CREATE TABLE person(\n  id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER, note TEXT)', strict: false, withoutRowid: false, columns: PERSON_COLUMNS },
      { name: 'tag', sql: 'CREATE TABLE tag(label TEXT)', strict: false, withoutRowid: false, columns: [column('label')] },
    ],
    views: [{ name: 'adults', sql: 'CREATE VIEW adults AS SELECT id, name FROM person', columns: [column('id', 'INTEGER'), column('name')] }],
    indexes: [{ name: 'person_name', table: 'person', unique: false, partial: false, sql: 'CREATE INDEX person_name ON person(name)', columns: ['name'] }],
    triggers: [{ name: 'person_added', table: 'person', sql: 'CREATE TRIGGER person_added AFTER INSERT ON person BEGIN SELECT 1; END' }],
  };
}

export function personPage(overrides: Partial<BrowseReply> = {}): BrowseReply {
  return {
    inTransaction: false,
    table: 'person',
    view: false,
    editable: true,
    columns: PERSON_COLUMNS,
    rows: [
      [1, 'Ada', 36, 'first'],
      [2, 'Bob', 17, null],
      [3, 'Cyd', 52, 'third'],
    ],
    keys: [[1], [2], [3]],
    total: 3,
    offset: 0,
    limit: 100,
    ...overrides,
  };
}

export function sqlReply(overrides: Partial<SqlReply> = {}): SqlReply {
  return { inTransaction: false, columns: [], rows: [], rowCount: 0, truncated: false, changes: 0, lastInsertId: 0, elapsedMs: 1.2, ...overrides };
}

/** The same as a SqlResult, the form Execute SQL consumes. */
export function sqlResult(overrides: Partial<SqlReply> = {}): SqlResult {
  return fromReply(sqlReply(overrides));
}

export function fakeInfo(overrides: Partial<DatabaseInfo> = {}): DatabaseInfo {
  return { path: '/data/people.sqldb', version: '0.2.0', readOnly: false, inTransaction: false, functions: ['count', 'lower'], ...overrides };
}

export const SAMPLE_PRAGMAS: PragmaInfo[] = [
  { name: 'foreign_keys', kind: 'boolean', scope: 'session', description: 'Enforce FOREIGN KEY constraints.', readOnly: false, value: 0, choices: [] },
  {
    name: 'temp_store',
    kind: 'choice',
    scope: 'session',
    description: 'Where a large ORDER BY puts its runs.',
    readOnly: false,
    value: 0,
    choices: [
      { value: 0, label: 'Default' },
      { value: 2, label: 'Memory' },
    ],
  },
  { name: 'user_version', kind: 'integer', scope: 'file', description: 'A number the application keeps in the file.', readOnly: false, value: 3, choices: [] },
  { name: 'journal_mode', kind: 'text', scope: 'store', description: 'Always wal.', readOnly: true, value: 'wal', choices: [] },
];

/** A DatabaseService whose calls are spies; the signals are real, so the views react. */
export function fakeDatabase() {
  return {
    info: signal<DatabaseInfo | null>(fakeInfo()),
    schema: signal<Schema>(sampleSchema()),
    inTransaction: signal(false),
    revision: signal(0),
    authRequired: signal(false),
    load: jasmine.createSpy('load').and.resolveTo(),
    loadSchema: jasmine.createSpy('loadSchema').and.resolveTo(),
    browse: jasmine.createSpy('browse').and.callFake(async () => personPage()),
    insertRow: jasmine.createSpy('insertRow').and.resolveTo(),
    updateRow: jasmine.createSpy('updateRow').and.resolveTo(),
    deleteRows: jasmine.createSpy('deleteRows').and.resolveTo(),
    execute: jasmine.createSpy('execute').and.callFake(async () => sqlReply()),
    commit: jasmine.createSpy('commit').and.resolveTo(),
    revert: jasmine.createSpy('revert').and.resolveTo(),
    pragmas: jasmine.createSpy('pragmas').and.callFake(async () => ({ inTransaction: false, pragmas: SAMPLE_PRAGMAS })),
    setPragma: jasmine.createSpy('setPragma').and.resolveTo(),
    dump: jasmine.createSpy('dump').and.resolveTo('BEGIN TRANSACTION;\nCOMMIT;\n'),
  };
}

export type FakeDatabase = ReturnType<typeof fakeDatabase>;

export function fakeNotice() {
  return { info: jasmine.createSpy('info'), error: jasmine.createSpy('error') };
}

export function databaseProviders(db: FakeDatabase, notice = fakeNotice()) {
  return [
    { provide: DatabaseService, useValue: db },
    { provide: NoticeService, useValue: notice },
  ];
}

/** A MatDialog that answers every open() with `result` instead of showing anything. */
export function dialogAnswering(result: unknown) {
  const open = jasmine.createSpy('open').and.returnValue({ afterClosed: () => of(result) });
  return {
    provider: { provide: MatDialog, useValue: { open } },
    open,
    /** The result of the dialogs opened from now on. */
    answer: (next: unknown) => open.and.returnValue({ afterClosed: () => of(next) }),
  };
}

/** Waits until the views have caught up with the signals. */
export async function settle(): Promise<void> {
  await TestBed.inject(ApplicationRef).whenStable();
}

export function query<T extends Element>(root: ParentNode, selector: string): T {
  const found = root.querySelector<T>(selector);
  if (!found) {
    throw new Error(`nothing matches ${selector}`);
  }
  return found;
}

export function typeInto(input: HTMLInputElement | HTMLTextAreaElement, text: string): void {
  input.value = text;
  input.dispatchEvent(new Event('input', { bubbles: true }));
}
