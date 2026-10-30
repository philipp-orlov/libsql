import { Component, inject, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatTabsModule } from '@angular/material/tabs';
import { MatTooltipModule } from '@angular/material/tooltip';

import { Cell, SqlResult } from '../../models/api.models';
import { DatabaseService } from '../../services/database.service';
import { DataGridComponent, GridCellDirective, GridColumn } from '../../shared/data-grid';
import { cellColumnOf, cellKind, rowIndexes, toCsvOf } from '../../utils/cell';
import { downloadText } from '../../utils/download';
import { SqlEditorComponent } from '../sql-editor/sql-editor.component';

export interface Editor {
  id: number;
  title: string;
  sql: string;
  running: boolean;
  result: SqlResult | null;
  /** What the message pane says; an error when `failed`. */
  message: string;
  failed: boolean;
}

export function describeRun(reply: SqlResult): string {
  const took = `${Math.round(reply.elapsedMs)}ms`;
  if (reply.columns.length > 0) {
    const shown = reply.truncated ? `, the first ${reply.rowCount} are shown` : '';
    return `${reply.totalRows} ${reply.totalRows === 1 ? 'row' : 'rows'} returned in ${took}${shown}`;
  }
  return `Query executed successfully, ${reply.changes} ${reply.changes === 1 ? 'row' : 'rows'} affected. Took ${took}`;
}

function newEditor(id: number): Editor {
  return { id, title: `SQL ${id}`, sql: '', running: false, result: null, message: '', failed: false };
}

/** The Execute SQL tab: editors in tabs, each with the result of its last run. */
@Component({
  selector: 'app-execute-sql',
  imports: [DataGridComponent, GridCellDirective, MatButtonModule, MatIconModule, MatTabsModule, MatTooltipModule, SqlEditorComponent],
  templateUrl: './execute-sql.component.html',
  styleUrl: './execute-sql.component.scss',
})
export class ExecuteSqlComponent {
  protected readonly db = inject(DatabaseService);

  protected readonly kindOf = (value: unknown) => cellKind(value as Cell);

  protected readonly editors = signal<Editor[]>([newEditor(1)]);
  protected readonly selected = signal(0);
  private nextId = 2;
  /** One set of columns per result, so the grid sees the same columns until the next run. */
  private readonly gridColumns = new WeakMap<SqlResult, GridColumn<number>[]>();
  /** The grid's data: a row is its index in the result, so cells are read only when shown. */
  private readonly gridRows = new WeakMap<SqlResult, number[]>();

  protected addEditor(): void {
    this.editors.update(editors => [...editors, newEditor(this.nextId++)]);
    this.selected.set(this.editors().length - 1);
  }

  protected closeEditor(id: number): void {
    this.editors.update(editors => {
      const rest = editors.filter(editor => editor.id !== id);
      return rest.length > 0 ? rest : [newEditor(this.nextId++)];
    });
    this.selected.update(index => Math.min(index, this.editors().length - 1));
  }

  protected setSql(id: number, sql: string): void {
    this.patch(id, { sql });
  }

  /** Runs what is selected in the editor, or all of it. */
  protected async run(editor: Editor, box: SqlEditorComponent): Promise<void> {
    const text = box.selectedText();
    if (text.trim() === '' || editor.running) {
      return;
    }
    this.patch(editor.id, { running: true });
    try {
      const result = await this.db.execute(text);
      this.patch(editor.id, { running: false, result, message: describeRun(result), failed: false });
    } catch (failure) {
      const message = failure instanceof Error ? failure.message : String(failure);
      this.patch(editor.id, { running: false, result: null, message: `Execution aborted: ${message}`, failed: true });
    }
  }

  protected columnsOf(result: SqlResult): GridColumn<number>[] {
    let columns = this.gridColumns.get(result);
    if (!columns) {
      // Result columns may share a name.
      columns = result.columns.map((name, i) => cellColumnOf(result, `c${i}`, name, i));
      this.gridColumns.set(result, columns);
    }
    return columns;
  }

  protected rowsOf(result: SqlResult): number[] {
    let rows = this.gridRows.get(result);
    if (!rows) {
      rows = rowIndexes(result);
      this.gridRows.set(result, rows);
    }
    return rows;
  }

  protected exportCsv(editor: Editor): void {
    if (editor.result) {
      downloadText(`${editor.title.replace(/\s+/g, '-').toLowerCase()}.csv`, toCsvOf(editor.result.columns, editor.result.rowCount, (r, c) => editor.result!.cell(r, c)), 'text/csv');
    }
  }

  protected async openFile(editor: Editor, input: HTMLInputElement): Promise<void> {
    const file = input.files?.[0];
    if (file) {
      this.patch(editor.id, { sql: await file.text(), title: file.name });
    }
    input.value = '';
  }

  protected saveFile(editor: Editor): void {
    downloadText(editor.title.endsWith('.sql') ? editor.title : `${editor.title.replace(/\s+/g, '-').toLowerCase()}.sql`, editor.sql, 'application/sql');
  }

  private patch(id: number, changes: Partial<Editor>): void {
    this.editors.update(editors => editors.map(editor => (editor.id === id ? { ...editor, ...changes } : editor)));
  }
}
