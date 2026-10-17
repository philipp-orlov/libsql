import { Component, computed, inject, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatCheckboxModule } from '@angular/material/checkbox';
import { MAT_DIALOG_DATA, MatDialogModule } from '@angular/material/dialog';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatIconModule } from '@angular/material/icon';
import { MatInputModule } from '@angular/material/input';
import { MatSelectModule } from '@angular/material/select';

import { TableInfo } from '../../models/api.models';
import { ColumnDraft, addColumnSql, dropColumnSql, emptyColumn, renameColumnSql, renameTableSql } from '../../utils/sql-builder';
import { COLUMN_TYPES } from '../create-table-dialog/create-table-dialog.component';

interface ExistingColumn {
  original: string;
  name: string;
  type: string;
  removed: boolean;
}

/** Renames a table, renames or drops its columns and adds new ones. Closes with the statements, one per line. */
@Component({
  selector: 'app-modify-table-dialog',
  imports: [MatButtonModule, MatCheckboxModule, MatDialogModule, MatFormFieldModule, MatIconModule, MatInputModule, MatSelectModule],
  templateUrl: './modify-table-dialog.component.html',
  styleUrl: './modify-table-dialog.component.scss',
})
export class ModifyTableDialogComponent {
  protected readonly table = inject<TableInfo>(MAT_DIALOG_DATA);
  protected readonly types = COLUMN_TYPES;

  protected readonly name = signal(this.table.name);
  protected readonly existing = signal<ExistingColumn[]>(
    this.table.columns.map(column => ({ original: column.name, name: column.name, type: column.type, removed: false })),
  );
  protected readonly added = signal<ColumnDraft[]>([]);

  /** What the dialog will run, in an order that keeps every name valid: columns first, the table last. */
  protected readonly statements = computed(() => {
    const original = this.table.name;
    const out: string[] = [];
    for (const column of this.existing()) {
      if (column.removed) {
        out.push(dropColumnSql(original, column.original));
      } else if (column.name.trim() !== '' && column.name !== column.original) {
        out.push(renameColumnSql(original, column.original, column.name));
      }
    }
    for (const column of this.added()) {
      out.push(addColumnSql(original, column));
    }
    if (this.name().trim() !== '' && this.name() !== original) {
      out.push(renameTableSql(original, this.name()));
    }
    return out;
  });
  protected readonly valid = computed(
    () =>
      this.statements().length > 0 &&
      this.name().trim() !== '' &&
      this.existing().every(column => column.removed || column.name.trim() !== '') &&
      this.added().every(column => column.name.trim() !== ''),
  );

  protected updateExisting(index: number, patch: Partial<ExistingColumn>): void {
    this.existing.update(columns => columns.map((column, i) => (i === index ? { ...column, ...patch } : column)));
  }

  protected updateAdded(index: number, patch: Partial<ColumnDraft>): void {
    this.added.update(columns => columns.map((column, i) => (i === index ? { ...column, ...patch } : column)));
  }

  protected add(): void {
    this.added.update(columns => [...columns, emptyColumn()]);
  }

  protected removeAdded(index: number): void {
    this.added.update(columns => columns.filter((_, i) => i !== index));
  }
}
