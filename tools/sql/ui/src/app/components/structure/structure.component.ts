import { Component, computed, inject, signal } from '@angular/core';
import { ComponentType } from '@angular/cdk/portal';
import { MatButtonModule } from '@angular/material/button';
import { MatDialog } from '@angular/material/dialog';
import { MatIconModule } from '@angular/material/icon';
import { MatTooltipModule } from '@angular/material/tooltip';

import { ColumnInfo, TableInfo } from '../../models/api.models';
import { DatabaseService } from '../../services/database.service';
import { NoticeService } from '../../services/notice.service';
import { WorkspaceService } from '../../services/workspace.service';
import { downloadText } from '../../utils/download';
import { ObjectKind, dropObjectSql, quoteIdentifier } from '../../utils/sql-builder';
import { ConfirmDialogComponent } from '../confirm-dialog/confirm-dialog.component';
import { CreateIndexDialogComponent } from '../create-index-dialog/create-index-dialog.component';
import { CreateTableDialogComponent } from '../create-table-dialog/create-table-dialog.component';
import { ModifyTableDialogComponent } from '../modify-table-dialog/modify-table-dialog.component';

export interface TreeRow {
  id: string;
  depth: number;
  kind: ObjectKind | 'group' | 'column';
  name: string;
  type: string;
  schema: string;
  expandable: boolean;
  /** The table a column belongs to. */
  table?: string;
}

function oneLine(sql: string): string {
  return sql.replace(/\s+/g, ' ').trim();
}

function columnSchema(column: ColumnInfo): string {
  const parts = [quoteIdentifier(column.name), column.type];
  if (column.primaryKey) {
    parts.push('PRIMARY KEY');
  }
  if (column.notNull) {
    parts.push('NOT NULL');
  }
  if (column.unique) {
    parts.push('UNIQUE');
  }
  if (column.defaultSql !== null) {
    parts.push(`DEFAULT ${column.defaultSql}`);
  }
  return parts.filter(Boolean).join(' ');
}

/** The Database Structure tab: tables, indices, views and triggers, with their columns. */
@Component({
  selector: 'app-structure',
  imports: [MatButtonModule, MatIconModule, MatTooltipModule],
  templateUrl: './structure.component.html',
  styleUrl: './structure.component.scss',
})
export class StructureComponent {
  protected readonly db = inject(DatabaseService);
  private readonly dialog = inject(MatDialog);
  private readonly notice = inject(NoticeService);
  private readonly workspace = inject(WorkspaceService);

  protected readonly expanded = signal<ReadonlySet<string>>(new Set(['group:tables']));
  protected readonly selectedId = signal<string | null>(null);
  protected readonly readOnly = computed(() => this.db.info()?.readOnly ?? false);

  protected readonly rows = computed<TreeRow[]>(() => {
    const schema = this.db.schema();
    const open = this.expanded();
    const out: TreeRow[] = [];
    const group = (key: string, title: string, count: number): boolean => {
      out.push({ id: `group:${key}`, depth: 0, kind: 'group', name: `${title} (${count})`, type: '', schema: '', expandable: count > 0 });
      return open.has(`group:${key}`);
    };

    if (group('tables', 'Tables', schema.tables.length)) {
      for (const table of schema.tables) {
        this.pushRelation(out, open, 'table', table.name, oneLine(table.sql), table.columns);
      }
    }
    if (group('indexes', 'Indices', schema.indexes.length)) {
      for (const index of schema.indexes) {
        out.push({ id: `index:${index.name}`, depth: 1, kind: 'index', name: index.name, type: '', schema: oneLine(index.sql), expandable: false, table: index.table });
      }
    }
    if (group('views', 'Views', schema.views.length)) {
      for (const view of schema.views) {
        this.pushRelation(out, open, 'view', view.name, oneLine(view.sql), view.columns);
      }
    }
    if (group('triggers', 'Triggers', schema.triggers.length)) {
      for (const trigger of schema.triggers) {
        out.push({ id: `trigger:${trigger.name}`, depth: 1, kind: 'trigger', name: trigger.name, type: '', schema: oneLine(trigger.sql), expandable: false, table: trigger.table });
      }
    }
    return out;
  });

  private readonly selected = computed(() => this.rows().find(row => row.id === this.selectedId()) ?? null);
  protected readonly selectedTable = computed<TableInfo | null>(() => {
    const row = this.selected();
    const name = row?.kind === 'table' ? row.name : row?.kind === 'column' ? row.table : undefined;
    return this.db.schema().tables.find(table => table.name === name) ?? null;
  });
  protected readonly deletable = computed(() => {
    const kind = this.selected()?.kind;
    return kind === 'table' || kind === 'view' || kind === 'index' || kind === 'trigger';
  });

  protected toggle(row: TreeRow): void {
    this.expanded.update(open => {
      const next = new Set(open);
      if (!next.delete(row.id)) {
        next.add(row.id);
      }
      return next;
    });
  }

  protected open(row: TreeRow): void {
    if (row.kind === 'table' || row.kind === 'view') {
      this.workspace.openTable(row.name);
    } else if (row.expandable) {
      this.toggle(row);
    }
  }

  protected async exportSql(): Promise<void> {
    try {
      const name = (this.db.info()?.path ?? 'database').split('/').pop()!.replace(/\.[^.]*$/, '');
      downloadText(`${name}.sql`, await this.db.dump(), 'application/sql');
    } catch (failure) {
      this.notice.error(failure);
    }
  }

  protected async createTable(): Promise<void> {
    const sql = await this.ask<string>(CreateTableDialogComponent, undefined, '900px');
    if (sql) {
      await this.run(sql);
    }
  }

  protected async createIndex(): Promise<void> {
    const sql = await this.ask<string>(CreateIndexDialogComponent, { tables: this.db.schema().tables, table: this.selectedTable()?.name ?? null }, '600px');
    if (sql) {
      await this.run(sql);
    }
  }

  protected async modifyTable(): Promise<void> {
    const table = this.selectedTable();
    const sql = table ? await this.ask<string>(ModifyTableDialogComponent, table, '640px') : undefined;
    if (sql) {
      await this.run(sql);
    }
  }

  protected async deleteSelected(): Promise<void> {
    const row = this.selected();
    if (!row || !this.deletable()) {
      return;
    }
    const kind = row.kind as ObjectKind;
    const confirmed = await this.ask<boolean>(ConfirmDialogComponent, {
      title: `Delete ${kind}`,
      message: `Are you sure you want to delete the ${kind} ${row.name}? This cannot be undone once the changes are written.`,
      action: 'Delete',
    });
    if (confirmed) {
      await this.run(dropObjectSql(kind, row.name));
      this.selectedId.set(null);
    }
  }

  private pushRelation(out: TreeRow[], open: ReadonlySet<string>, kind: 'table' | 'view', name: string, sql: string, columns: ColumnInfo[]): void {
    const id = `${kind}:${name}`;
    out.push({ id, depth: 1, kind, name, type: '', schema: sql, expandable: columns.length > 0 });
    if (open.has(id)) {
      for (const column of columns) {
        out.push({ id: `column:${name}.${column.name}`, depth: 2, kind: 'column', name: column.name, type: column.type, schema: columnSchema(column), expandable: false, table: name });
      }
    }
  }

  private ask<R>(component: ComponentType<unknown>, data: unknown, width?: string): Promise<R | undefined> {
    return new Promise(resolve => {
      this.dialog.open(component, { data, width, maxWidth: '95vw' }).afterClosed().subscribe((result: R | undefined) => resolve(result));
    });
  }

  private async run(sql: string): Promise<void> {
    try {
      await this.db.execute(sql);
    } catch (failure) {
      this.notice.error(failure);
    }
  }
}
