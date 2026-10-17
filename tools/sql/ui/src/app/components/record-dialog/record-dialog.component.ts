import { Component, computed, inject, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MAT_DIALOG_DATA, MatDialogModule } from '@angular/material/dialog';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';

import { Cell, ColumnInfo } from '../../models/api.models';
import { inputToCell } from '../../utils/cell';

export interface RecordData {
  table: string;
  columns: ColumnInfo[];
}

/** Asks for the values of a new row. A column left empty takes its default. Closes with the values given. */
@Component({
  selector: 'app-record-dialog',
  imports: [MatButtonModule, MatDialogModule, MatFormFieldModule, MatInputModule],
  templateUrl: './record-dialog.component.html',
  styleUrl: './record-dialog.component.scss',
})
export class RecordDialogComponent {
  protected readonly data = inject<RecordData>(MAT_DIALOG_DATA);

  // Generated columns cannot be written.
  protected readonly columns = this.data.columns.filter(column => !column.generated);
  private readonly texts = signal<Record<string, string>>({});

  protected readonly values = computed(() => {
    const out: Record<string, Cell> = {};
    for (const column of this.columns) {
      const text = this.texts()[column.name] ?? '';
      if (text !== '') {
        out[column.name] = inputToCell(text, column);
      }
    }
    return out;
  });

  protected hint(column: ColumnInfo): string {
    if (column.defaultSql !== null) {
      return `default ${column.defaultSql}`;
    }
    return column.primaryKey ? 'assigned automatically' : column.notNull ? 'required' : 'NULL';
  }

  protected set(name: string, text: string): void {
    this.texts.update(texts => ({ ...texts, [name]: text }));
  }
}
