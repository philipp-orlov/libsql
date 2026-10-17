import { Component, computed, inject, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatCheckboxModule } from '@angular/material/checkbox';
import { MAT_DIALOG_DATA, MatDialogModule } from '@angular/material/dialog';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatSelectModule } from '@angular/material/select';

import { TableInfo } from '../../models/api.models';
import { createIndexSql } from '../../utils/sql-builder';

export interface CreateIndexData {
  tables: TableInfo[];
  /** The table to start with. */
  table: string | null;
}

interface IndexColumn {
  name: string;
  used: boolean;
  descending: boolean;
}

/** Closes with the CREATE INDEX statement. */
@Component({
  selector: 'app-create-index-dialog',
  imports: [MatButtonModule, MatCheckboxModule, MatDialogModule, MatFormFieldModule, MatInputModule, MatSelectModule],
  templateUrl: './create-index-dialog.component.html',
  styleUrl: './create-index-dialog.component.scss',
})
export class CreateIndexDialogComponent {
  protected readonly data = inject<CreateIndexData>(MAT_DIALOG_DATA);

  protected readonly name = signal('');
  protected readonly table = signal(this.data.table ?? this.data.tables[0]?.name ?? '');
  protected readonly unique = signal(false);
  protected readonly columns = signal<IndexColumn[]>(this.columnsOf(this.table()));

  protected readonly sql = computed(() =>
    createIndexSql({
      name: this.name(),
      table: this.table(),
      unique: this.unique(),
      columns: this.columns().filter(column => column.used),
    }),
  );
  protected readonly valid = computed(() => this.name().trim() !== '' && this.columns().some(column => column.used));

  protected selectTable(table: string): void {
    this.table.set(table);
    this.columns.set(this.columnsOf(table));
  }

  protected update(index: number, patch: Partial<IndexColumn>): void {
    this.columns.update(columns => columns.map((column, i) => (i === index ? { ...column, ...patch } : column)));
  }

  private columnsOf(table: string): IndexColumn[] {
    const info = this.data.tables.find(candidate => candidate.name === table);
    return (info?.columns ?? []).map(column => ({ name: column.name, used: false, descending: false }));
  }
}
