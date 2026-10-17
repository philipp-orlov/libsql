import { Component, computed, effect, inject, signal, untracked } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatDialog } from '@angular/material/dialog';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatIconModule } from '@angular/material/icon';
import { MatPaginatorModule, PageEvent } from '@angular/material/paginator';
import { MatSelectModule } from '@angular/material/select';
import { MatTableModule } from '@angular/material/table';
import { MatTooltipModule } from '@angular/material/tooltip';

import { AutofocusDirective } from '../../directives/autofocus.directive';
import { BrowseReply, Cell, SortSpec } from '../../models/api.models';
import { DatabaseService } from '../../services/database.service';
import { NoticeService } from '../../services/notice.service';
import { WorkspaceService } from '../../services/workspace.service';
import { cellEditText, cellKind, cellText, inputToCell, isEditableCell, toCsv } from '../../utils/cell';
import { downloadText } from '../../utils/download';
import { ConfirmDialogComponent } from '../confirm-dialog/confirm-dialog.component';
import { RecordDialogComponent } from '../record-dialog/record-dialog.component';

const PAGE_SIZES = [50, 100, 500, 1000];
const FILTER_DELAY_MS = 300;
const EXPORT_PAGE_ROWS = 1000;
const EXPORT_LIMIT = 100000;

interface GridRow {
  index: number;
  cells: Cell[];
}

interface CellRef {
  row: number;
  column: number;
}

/** The Browse Data tab: one table or view as a grid that can be sorted, filtered, paged and, for a table, edited. */
@Component({
  selector: 'app-browse',
  imports: [AutofocusDirective, MatButtonModule, MatFormFieldModule, MatIconModule, MatPaginatorModule, MatSelectModule, MatTableModule, MatTooltipModule],
  templateUrl: './browse.component.html',
  styleUrl: './browse.component.scss',
})
export class BrowseComponent {
  private readonly db = inject(DatabaseService);
  private readonly dialog = inject(MatDialog);
  private readonly notice = inject(NoticeService);
  private readonly workspace = inject(WorkspaceService);

  protected readonly pageSizes = PAGE_SIZES;
  protected readonly cellText = cellText;
  protected readonly cellEditText = cellEditText;
  protected readonly cellKind = cellKind;

  protected readonly table = this.workspace.table;
  protected readonly tableNames = computed(() => [
    ...this.db.schema().tables.map(table => table.name),
    ...this.db.schema().views.map(view => view.name),
  ]);

  protected readonly page = signal<BrowseReply | null>(null);
  protected readonly pageIndex = signal(0);
  protected readonly pageSize = signal(PAGE_SIZES[1]);
  protected readonly sort = signal<SortSpec | null>(null);
  protected readonly filters = signal<Record<string, string>>({});
  protected readonly selectedRows = signal<ReadonlySet<number>>(new Set());
  protected readonly activeCell = signal<CellRef | null>(null);
  protected readonly editing = signal<(CellRef & { text: string }) | null>(null);

  protected readonly rows = computed<GridRow[]>(() => (this.page()?.rows ?? []).map((cells, index) => ({ index, cells })));
  protected readonly columnIds = computed(() => (this.page()?.columns ?? []).map((_, i) => `c${i}`));
  protected readonly dataColumns = computed(() => ['rowno', ...this.columnIds()]);
  protected readonly filterColumns = computed(() => ['filter-rowno', ...this.columnIds().map(id => `filter-${id}`)]);
  protected readonly editable = computed(() => this.page()?.editable ?? false);

  protected readonly panelOpen = signal(false);
  protected readonly panelText = signal('');
  protected readonly exporting = signal(false);
  protected readonly activeValue = computed(() => {
    const active = this.activeCell();
    return active ? this.page()?.rows[active.row]?.[active.column] : undefined;
  });
  protected readonly activeColumn = computed(() => {
    const active = this.activeCell();
    return active ? (this.page()?.columns[active.column] ?? null) : null;
  });
  protected readonly panelEditable = computed(() => {
    const value = this.activeValue();
    return this.editable() && value !== undefined && isEditableCell(value);
  });
  protected readonly panelChanged = computed(() => {
    const value = this.activeValue();
    return value !== undefined && this.panelText() !== cellEditText(value);
  });

  private loadCount = 0;
  private filterTimer: ReturnType<typeof setTimeout> | undefined;
  private anchorRow = 0;
  private seenRevision = this.db.revision();

  constructor() {
    effect(() => {
      const table = this.table();
      untracked(() => {
        this.pageIndex.set(0);
        this.sort.set(null);
        this.filters.set({});
        void this.load(table);
      });
    });
    effect(() => {
      const revision = this.db.revision();
      untracked(() => {
        if (revision !== this.seenRevision) {
          this.seenRevision = revision;
          void this.load(this.table());
        }
      });
    });
    // The panel shows the cell that is active.
    effect(() => {
      const value = this.activeValue();
      untracked(() => this.panelText.set(value === undefined ? '' : cellEditText(value)));
    });
    // A table that was dropped or renamed is no longer there to browse.
    effect(() => {
      const table = this.table();
      if (table !== null && !this.tableNames().includes(table) && this.db.schema().tables.length + this.db.schema().views.length > 0) {
        untracked(() => this.workspace.table.set(null));
      }
    });
  }

  protected async load(table: string | null = this.table()): Promise<void> {
    const id = ++this.loadCount;
    if (table === null) {
      this.page.set(null);
      return;
    }
    const sort = this.sort();
    try {
      const reply = await this.db.browse({
        table,
        offset: this.pageIndex() * this.pageSize(),
        limit: this.pageSize(),
        sort: sort ? [sort] : [],
        filters: this.filters(),
      });
      if (id !== this.loadCount) {
        return;
      }
      this.page.set(reply);
      this.selectedRows.set(new Set());
      this.activeCell.set(null);
      this.editing.set(null);
    } catch (failure) {
      if (id === this.loadCount) {
        this.page.set(null);
        this.notice.error(failure);
      }
    }
  }

  protected selectTable(name: string): void {
    this.workspace.table.set(name);
  }

  protected onPage(event: PageEvent): void {
    this.pageIndex.set(event.pageIndex);
    this.pageSize.set(event.pageSize);
    void this.load();
  }

  protected sortState(columnName: string): 'asc' | 'desc' | null {
    const sort = this.sort();
    return sort?.column === columnName ? (sort.descending ? 'desc' : 'asc') : null;
  }

  /** Ascending, then descending, then the table's own order. */
  protected toggleSort(columnName: string): void {
    const state = this.sortState(columnName);
    this.sort.set(state === null ? { column: columnName, descending: false } : state === 'asc' ? { column: columnName, descending: true } : null);
    this.pageIndex.set(0);
    void this.load();
  }

  protected setFilter(columnName: string, text: string): void {
    this.filters.update(filters => {
      const next = { ...filters };
      if (text.trim() === '') {
        delete next[columnName];
      } else {
        next[columnName] = text;
      }
      return next;
    });
    clearTimeout(this.filterTimer);
    this.filterTimer = setTimeout(() => {
      this.pageIndex.set(0);
      void this.load();
    }, FILTER_DELAY_MS);
  }

  protected selectCell(row: number, column: number, event: MouseEvent): void {
    this.activeCell.set({ row, column });
    this.selectRow(row, event);
  }

  protected selectRow(row: number, event: MouseEvent): void {
    const selection = new Set(event.ctrlKey || event.metaKey ? this.selectedRows() : []);
    if (event.shiftKey) {
      for (let i = Math.min(this.anchorRow, row); i <= Math.max(this.anchorRow, row); i++) {
        selection.add(i);
      }
    } else if ((event.ctrlKey || event.metaKey) && selection.has(row)) {
      selection.delete(row);
    } else {
      selection.add(row);
      this.anchorRow = row;
    }
    this.selectedRows.set(selection);
  }

  protected isActive(row: number, column: number): boolean {
    const active = this.activeCell();
    return active?.row === row && active.column === column;
  }

  protected isEditing(row: number, column: number): boolean {
    const edit = this.editing();
    return edit?.row === row && edit.column === column;
  }

  protected startEdit(row: number, column: number): void {
    const cell = this.page()?.rows[row]?.[column];
    if (this.editable() && cell !== undefined && isEditableCell(cell)) {
      this.editing.set({ row, column, text: cellEditText(cell) });
    }
  }

  protected cancelEdit(): void {
    this.editing.set(null);
  }

  protected editText(text: string): void {
    this.editing.update(edit => (edit ? { ...edit, text } : edit));
  }

  protected async commitEdit(): Promise<void> {
    const edit = this.editing();
    const page = this.page();
    if (!edit || !page) {
      return;
    }
    this.editing.set(null);
    const column = page.columns[edit.column];
    const original = page.rows[edit.row][edit.column];
    if (edit.text === cellEditText(original)) {
      return;
    }
    await this.update(edit.row, column.name, inputToCell(edit.text, column));
  }

  protected async setNull(): Promise<void> {
    const active = this.activeCell();
    const page = this.page();
    if (active && page && isEditableCell(page.rows[active.row][active.column])) {
      await this.update(active.row, page.columns[active.column].name, null);
    }
  }

  protected async applyPanel(): Promise<void> {
    const active = this.activeCell();
    const column = this.activeColumn();
    if (active && column && this.panelEditable() && this.panelChanged()) {
      await this.update(active.row, column.name, inputToCell(this.panelText(), column));
    }
  }

  /** Saves every row the filter matches, the way the grid shows them, up to a limit. */
  protected async exportCsv(): Promise<void> {
    const page = this.page();
    if (!page || this.exporting()) {
      return;
    }
    this.exporting.set(true);
    try {
      const sort = this.sort();
      const wanted = Math.min(page.total, EXPORT_LIMIT);
      const rows: Cell[][] = [];
      for (let offset = 0; offset < wanted; offset += EXPORT_PAGE_ROWS) {
        const part = await this.db.browse({ table: page.table, offset, limit: EXPORT_PAGE_ROWS, sort: sort ? [sort] : [], filters: this.filters() });
        rows.push(...part.rows);
      }
      downloadText(`${page.table}.csv`, toCsv(page.columns.map(column => column.name), rows), 'text/csv');
      if (page.total > EXPORT_LIMIT) {
        this.notice.info(`Only the first ${EXPORT_LIMIT} of ${page.total} rows were exported.`);
      }
    } catch (failure) {
      this.notice.error(failure);
    } finally {
      this.exporting.set(false);
    }
  }

  protected async addRecord(): Promise<void> {
    const page = this.page();
    if (!page) {
      return;
    }
    const values = await this.ask<Record<string, Cell>>(RecordDialogComponent, { table: page.table, columns: page.columns });
    // Cancel closes the dialog with an empty string, or with nothing.
    if (!values) {
      return;
    }
    try {
      await this.db.insertRow(page.table, values);
      this.pageIndex.set(Math.floor(page.total / this.pageSize()));
      await this.load();
    } catch (failure) {
      this.notice.error(failure);
    }
  }

  protected async deleteRecords(): Promise<void> {
    const page = this.page();
    const rows = [...this.selectedRows()].sort((a, b) => a - b);
    if (!page || rows.length === 0) {
      return;
    }
    const confirmed = await this.ask<boolean>(ConfirmDialogComponent, {
      title: 'Delete records',
      message: `Are you sure you want to delete ${rows.length === 1 ? 'the selected record' : `the ${rows.length} selected records`}?`,
      action: 'Delete',
    });
    if (!confirmed) {
      return;
    }
    try {
      await this.db.deleteRows(page.table, rows.map(row => page.keys[row]));
    } catch (failure) {
      this.notice.error(failure);
    }
  }

  private async update(row: number, column: string, value: Cell): Promise<void> {
    const page = this.page();
    if (!page) {
      return;
    }
    try {
      await this.db.updateRow(page.table, page.keys[row], { [column]: value });
    } catch (failure) {
      this.notice.error(failure);
    }
  }

  private ask<T>(component: Parameters<MatDialog['open']>[0], data: unknown): Promise<T | undefined> {
    return new Promise(resolve => {
      this.dialog.open(component, { data }).afterClosed().subscribe((result: T | undefined) => resolve(result));
    });
  }
}
