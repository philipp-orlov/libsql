// Copyright (c) 2026 Philipp Orlov

import { TextComponent } from '../shared/text.component';
import { Component, inject, input, output, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';

import { SqlEditorComponent } from '../../components/sql-editor/sql-editor.component';
import { DatabaseService } from '../../services/database.service';
import { bindNames, DataSet, defaultParams, ReportDef, formatValue } from '../core';
import { ReportService } from '../report.service';
import { assign, Edit, unique } from './editing';

const SAMPLE_ROWS = 50;

/** The queries a report takes its rows from. Run one to see its first rows and to learn its columns. */
@Component({
  selector: 'app-datasets-editor',
  imports: [MatButtonModule, MatIconModule, SqlEditorComponent, TextComponent],
  templateUrl: './datasets-editor.component.html',
  styleUrl: './editor.scss',
})
export class DatasetsEditorComponent {
  readonly report = input.required<ReportDef>();
  readonly edit = output<Edit>();
  /** A dataset was run: its columns are what the layout editor offers. */
  readonly ran = output<{ name: string; data: DataSet }>();

  protected readonly db = inject(DatabaseService);
  private readonly svc = inject(ReportService);

  protected readonly selected = signal(0);
  protected readonly sample = signal<{ name: string; data: DataSet; truncated: boolean } | null>(null);
  protected readonly error = signal('');
  protected readonly running = signal(false);

  protected get index(): number {
    return Math.min(this.selected(), this.report().datasets.length - 1);
  }

  protected add(): void {
    this.edit.emit(r => r.datasets.push({ name: unique('data', r.datasets.map(d => d.name)), sql: 'SELECT ' }));
    this.selected.set(this.report().datasets.length);
  }

  protected remove(i: number): void {
    this.edit.emit(r => void r.datasets.splice(i, 1));
    this.sample.set(null);
  }

  protected rename(i: number, name: string): void {
    const old = this.report().datasets[i].name;
    this.edit.emit(r => {
      r.datasets[i].name = name;
      // items that used the dataset follow it
      for (const it of [...r.body, ...r.header, ...r.footer]) if ('dataset' in it && it.dataset === old) it.dataset = name;
    });
  }

  protected setSql(i: number, sql: string): void {
    if (sql !== this.report().datasets[i]?.sql) this.edit.emit(r => assign(r.datasets[i], 'sql', sql));
  }

  protected usedParameters(sql: string): string[] {
    return bindNames(sql).names;
  }

  protected async run(i: number): Promise<void> {
    const d = this.report().datasets[i];
    this.running.set(true);
    this.error.set('');
    try {
      const { data, truncated } = await this.svc.query(d.sql, defaultParams(this.report()), SAMPLE_ROWS);
      this.sample.set({ name: d.name, data, truncated });
      this.ran.emit({ name: d.name, data });
    } catch (e) {
      this.sample.set(null);
      this.error.set((e as Error).message);
    } finally {
      this.running.set(false);
    }
  }

  protected isNumber(v: unknown): boolean {
    return typeof v === 'number';
  }

  protected text(v: unknown): string {
    return formatValue(v as never);
  }
}
