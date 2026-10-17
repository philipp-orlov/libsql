import { Component, inject, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatTableModule } from '@angular/material/table';
import { MatTabsModule } from '@angular/material/tabs';
import { MatTooltipModule } from '@angular/material/tooltip';

import { SqlReply } from '../../models/api.models';
import { DatabaseService } from '../../services/database.service';
import { cellKind, cellText, toCsv } from '../../utils/cell';
import { downloadText } from '../../utils/download';

export interface Editor {
  id: number;
  title: string;
  sql: string;
  running: boolean;
  result: SqlReply | null;
  /** What the message pane says; an error when `failed`. */
  message: string;
  failed: boolean;
}

export function describeRun(reply: SqlReply): string {
  const took = `${Math.round(reply.elapsedMs)}ms`;
  if (reply.columns.length > 0) {
    const shown = reply.truncated ? `, the first ${reply.rows.length} are shown` : '';
    return `${reply.rowCount} ${reply.rowCount === 1 ? 'row' : 'rows'} returned in ${took}${shown}`;
  }
  return `Query executed successfully, ${reply.changes} ${reply.changes === 1 ? 'row' : 'rows'} affected. Took ${took}`;
}

function newEditor(id: number): Editor {
  return { id, title: `SQL ${id}`, sql: '', running: false, result: null, message: '', failed: false };
}

/** The Execute SQL tab: editors in tabs, each with the result of its last run. */
@Component({
  selector: 'app-execute-sql',
  imports: [MatButtonModule, MatIconModule, MatTableModule, MatTabsModule, MatTooltipModule],
  templateUrl: './execute-sql.component.html',
  styleUrl: './execute-sql.component.scss',
})
export class ExecuteSqlComponent {
  private readonly db = inject(DatabaseService);

  protected readonly cellText = cellText;
  protected readonly cellKind = cellKind;

  protected readonly editors = signal<Editor[]>([newEditor(1)]);
  protected readonly selected = signal(0);
  private nextId = 2;

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

  /** Runs what is selected in the box, or all of it. */
  protected async run(editor: Editor, box: HTMLTextAreaElement): Promise<void> {
    const text = box.selectionStart !== box.selectionEnd ? box.value.substring(box.selectionStart, box.selectionEnd) : box.value;
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

  protected columnIds(editor: Editor): string[] {
    return (editor.result?.columns ?? []).map((_, i) => `c${i}`);
  }

  protected exportCsv(editor: Editor): void {
    if (editor.result) {
      downloadText(`${editor.title.replace(/\s+/g, '-').toLowerCase()}.csv`, toCsv(editor.result.columns, editor.result.rows), 'text/csv');
    }
  }

  protected onKeydown(event: KeyboardEvent, editor: Editor, box: HTMLTextAreaElement): void {
    if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
      event.preventDefault();
      void this.run(editor, box);
    }
  }

  private patch(id: number, changes: Partial<Editor>): void {
    this.editors.update(editors => editors.map(editor => (editor.id === id ? { ...editor, ...changes } : editor)));
  }
}
