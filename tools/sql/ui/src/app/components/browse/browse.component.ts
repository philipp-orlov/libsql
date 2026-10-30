import { Component, computed, effect, inject, signal, untracked } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatDialog } from '@angular/material/dialog';
import { MatInputModule } from '@angular/material/input';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatIconModule } from '@angular/material/icon';
import { MatSelectModule } from '@angular/material/select';
import { MatTooltipModule } from '@angular/material/tooltip';

import { AutofocusDirective } from '../../directives/autofocus.directive';
import { BrowseReply, Cell, SortSpec } from '../../models/api.models';
import { DatabaseService } from '../../services/database.service';
import { NoticeService } from '../../services/notice.service';
import { WorkspaceService } from '../../services/workspace.service';
import { CellRef, DataGridComponent, GridCellDirective, GroupDescriptor, SortDescriptor, effectiveSort } from '../../shared/data-grid';
import { cellColumn, cellEditText, cellKind, cellText, inputToCell, isEditableCell, toCsv } from '../../utils/cell';
import { downloadBlob, downloadText } from '../../utils/download';
import { ExportFormat, ReportService } from '../../reports/report.service';
import { ConfirmDialogComponent } from '../confirm-dialog/confirm-dialog.component';
import { RecordDialogComponent } from '../record-dialog/record-dialog.component';

const PAGE_SIZES = [50, 100, 500, 1000];
const EXPORT_PAGE_ROWS = 1000;
const EXPORT_LIMIT = 100000;

/** The Browse Data tab: one table or view as a grid that can be sorted, grouped, filtered, paged and, for a table, edited. */
@Component({
  selector: 'app-browse',
  imports: [MatInputModule, AutofocusDirective, DataGridComponent, GridCellDirective, MatButtonModule, MatFormFieldModule, MatIconModule, MatSelectModule, MatTooltipModule],
  templateUrl: './browse.component.html',
  styleUrl: './browse.component.scss',
})
export class BrowseComponent {
  private readonly db = inject(DatabaseService);
  private readonly dialog = inject(MatDialog);
  private readonly notice = inject(NoticeService);
  private readonly workspace = inject(WorkspaceService);
  protected readonly reports = inject(ReportService);

  protected readonly pageSizes = PAGE_SIZES;
  protected readonly cellText = cellText;
  protected readonly cellEditText = cellEditText;
  protected readonly cellKind = cellKind;
  protected readonly kindOf = (value: unknown) => cellKind(value as Cell);

  protected readonly table = this.workspace.table;
  protected readonly tableNames = computed(() => [
    ...this.db.schema().tables.map(table => table.name),
    ...this.db.schema().views.map(view => view.name),
  ]);

  protected readonly page = signal<BrowseReply | null>(null);
  protected readonly pageIndex = signal(0);
  protected readonly pageSize = signal(PAGE_SIZES[1]);
  protected readonly sort = signal<SortDescriptor[]>([]);
  protected readonly group = signal<GroupDescriptor[]>([]);
  protected readonly filters = signal<Record<string, string>>({});
  protected readonly selectedRows = signal<ReadonlySet<number>>(new Set());
  /** The field of a cell is the name of its column. */
  protected readonly activeCell = signal<CellRef | null>(null);
  protected readonly editing = signal<(CellRef & { text: string }) | null>(null);

  protected readonly gridColumns = computed(() => (this.page()?.columns ?? []).map((column, i) => cellColumn(column.name, column.name, i)));
  protected readonly editable = computed(() => this.page()?.editable ?? false);

  protected readonly panelOpen = signal(false);
  protected readonly panelText = signal('');
  protected readonly exporting = signal(false);
  protected readonly activeValue = computed(() => {
    const active = this.activeCell();
    const page = this.page();
    return active && page ? page.rows[active.row]?.[this.columnIndex(active.field)] : undefined;
  });
  protected readonly activeColumn = computed(() => {
    const active = this.activeCell();
    return active ? (this.page()?.columns.find(column => column.name === active.field) ?? null) : null;
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
  private seenRevision = this.db.revision();

  constructor() {
    effect(() => {
      const table = this.table();
      untracked(() => {
        this.pageIndex.set(0);
        this.sort.set([]);
        this.group.set([]);
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
    try {
      const reply = await this.db.browse({
        table,
        offset: this.pageIndex() * this.pageSize(),
        limit: this.pageSize(),
        sort: this.sortSpecs(),
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

  /** The grouped columns sort first, so a group is not split across the pages it spans. */
  private sortSpecs(): SortSpec[] {
    return effectiveSort(this.group(), this.sort()).map(s => ({ column: s.field, descending: s.dir === 'desc' }));
  }

  private columnIndex(field: string): number {
    return this.page()?.columns.findIndex(column => column.name === field) ?? -1;
  }

  protected isEditing(row: number, field: string): boolean {
    const edit = this.editing();
    return edit?.row === row && edit.field === field;
  }

  protected startEdit(row: number, field: string): void {
    const cell = this.page()?.rows[row]?.[this.columnIndex(field)];
    if (this.editable() && cell !== undefined && isEditableCell(cell)) {
      this.editing.set({ row, field, text: cellEditText(cell) });
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
    const index = this.columnIndex(edit.field);
    const column = page.columns[index];
    const original = page.rows[edit.row][index];
    if (edit.text === cellEditText(original)) {
      return;
    }
    await this.update(edit.row, column.name, inputToCell(edit.text, column));
  }

  protected async setNull(): Promise<void> {
    const active = this.activeCell();
    const value = this.activeValue();
    if (active && value !== undefined && isEditableCell(value)) {
      await this.update(active.row, active.field, null);
    }
  }

  protected async applyPanel(): Promise<void> {
    const active = this.activeCell();
    const column = this.activeColumn();
    if (active && column && this.panelEditable() && this.panelChanged()) {
      await this.update(active.row, column.name, inputToCell(this.panelText(), column));
    }
  }

  /** Every row the filter matches, in the order the grid shows them, up to a limit. */
  private async exportRows(page: BrowseReply): Promise<Cell[][]> {
    const sort = this.sortSpecs();
    const wanted = Math.min(page.total, EXPORT_LIMIT);
    const rows: Cell[][] = [];
    for (let offset = 0; offset < wanted; offset += EXPORT_PAGE_ROWS) {
      const part = await this.db.browse({ table: page.table, offset, limit: EXPORT_PAGE_ROWS, sort, filters: this.filters() });
      rows.push(...part.rows);
    }
    if (page.total > EXPORT_LIMIT) {
      this.notice.info(`Only the first ${EXPORT_LIMIT} of ${page.total} rows were exported.`);
    }
    return rows;
  }

  /** Saves the rows the filter matches as CSV. */
  protected async exportCsv(): Promise<void> {
    await this.exportAs(async (page, rows) => downloadText(`${page.table}.csv`, toCsv(page.columns.map(column => column.name), rows), 'text/csv'));
  }

  /** Saves the rows the filter matches as a PDF, an Excel workbook or a Word document, the way a report would. */
  protected async exportReport(format: ExportFormat): Promise<void> {
    await this.exportAs(async (page, rows) => {
      const blob = await this.reports.exportRows(format, { title: page.table, columns: page.columns.map(column => column.name), rows });
      downloadBlob(`${page.table}.${format}`, blob);
    });
  }

  private async exportAs(save: (page: BrowseReply, rows: Cell[][]) => Promise<void>): Promise<void> {
    const page = this.page();
    if (!page || this.exporting()) {
      return;
    }
    this.exporting.set(true);
    try {
      await save(page, await this.exportRows(page));
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
