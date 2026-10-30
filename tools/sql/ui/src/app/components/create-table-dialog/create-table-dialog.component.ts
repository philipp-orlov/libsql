import { Component, computed, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatCheckboxModule } from '@angular/material/checkbox';
import { MatDialogModule } from '@angular/material/dialog';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatIconModule } from '@angular/material/icon';
import { MatInputModule } from '@angular/material/input';
import { MatSelectModule } from '@angular/material/select';

import { ColumnDraft, createTableSql, emptyColumn } from '../../utils/sql-builder';

export const COLUMN_TYPES = ['INTEGER', 'TEXT', 'REAL', 'NUMERIC', 'BLOB', 'DATETIME'];

/** Edits a new table the way DB Browser's Create Table window does. Closes with the CREATE TABLE statement. */
@Component({
  selector: 'app-create-table-dialog',
  imports: [MatButtonModule, MatCheckboxModule, MatDialogModule, MatFormFieldModule, MatIconModule, MatInputModule, MatSelectModule],
  templateUrl: './create-table-dialog.component.html',
  styleUrl: './create-table-dialog.component.scss',
})
export class CreateTableDialogComponent {
  protected readonly types = COLUMN_TYPES;
  protected readonly name = signal('');
  protected readonly withoutRowid = signal(false);
  protected readonly strict = signal(false);
  protected readonly columns = signal<ColumnDraft[]>([
    { ...emptyColumn(), name: 'id', type: 'INTEGER', notNull: true, primaryKey: true, autoIncrement: true },
  ]);

  protected readonly sql = computed(() =>
    createTableSql({ name: this.name(), columns: this.columns(), withoutRowid: this.withoutRowid(), strict: this.strict() }),
  );
  protected readonly valid = computed(
    () => this.name().trim() !== '' && this.columns().length > 0 && this.columns().every(column => column.name.trim() !== ''),
  );

  protected update(index: number, patch: Partial<ColumnDraft>): void {
    this.columns.update(columns => columns.map((column, i) => (i === index ? { ...column, ...patch } : column)));
  }

  protected add(): void {
    this.columns.update(columns => [...columns, emptyColumn()]);
  }

  protected remove(index: number): void {
    this.columns.update(columns => columns.filter((_, i) => i !== index));
  }
}
