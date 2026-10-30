// Copyright (c) 2026 Philipp Orlov

import { Component, OnInit, computed, effect, inject, signal, untracked } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatCheckboxModule } from '@angular/material/checkbox';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatMenuModule } from '@angular/material/menu';

import { NoticeService } from '../../services/notice.service';
import { DatabaseService } from '../../services/database.service';
import { downloadBlob, downloadText } from '../../utils/download';
import { LayoutResult, ParamDef, ParamOption, paramText, paramValue, ReportDef, tableData, tableToCsv, TableData, Value, ViewState } from '../core';
import { ReportService, RunResult } from '../report.service';
import { Choice, SelectComponent } from '../shared/select.component';
import { DateComponent } from '../shared/date.component';
import { DrillRequest, ReportViewerComponent } from '../viewer/report-viewer.component';

/** Lists the reports, asks for the parameters, runs one, shows it page by page, and exports it as PDF or CSV. */
@Component({
  selector: 'app-report-runner',
  imports: [MatButtonModule, MatCheckboxModule, MatFormFieldModule, MatIconModule, MatInputModule, MatMenuModule, ReportViewerComponent, SelectComponent, DateComponent],
  templateUrl: './report-runner.component.html',
  styleUrl: './report-runner.component.scss',
})
export class ReportRunnerComponent implements OnInit {
  protected readonly svc = inject(ReportService);
  private readonly db = inject(DatabaseService);
  private readonly notice = inject(NoticeService);

  protected readonly reports = signal<ReportDef[]>([]);
  protected readonly problems = signal<string[]>([]);
  protected readonly loading = signal(true);
  protected readonly selected = signal<ReportDef | null>(null);
  /** What the form holds, as text (checkboxes as 'true'/''): converted when the report runs. */
  protected readonly form = signal<Record<string, string>>({});
  protected readonly choices = signal<Record<string, ParamOption[]>>({});
  protected readonly running = signal(false);
  protected readonly layout = signal<LayoutResult | null>(null);
  protected readonly warnings = signal<string[]>([]);
  protected readonly error = signal('');
  protected readonly tables = signal<TableData[]>([]);
  /** What the reader did in the preview: series switched off, a sort, folded groups. The PDF and the CSV follow it. */
  protected readonly view = signal<ViewState>({});

  /** The reports the reader drilled through from, oldest first, with the parameters each was run with. */
  protected readonly trail = signal<{ report: ReportDef; form: Record<string, string> }[]>([]);

  private lastRun: RunResult | null = null;
  private lastValues: Record<string, Value> = {};

  protected readonly pages = computed(() => this.layout()?.pages ?? []);
  protected readonly missing = computed(() => {
    const r = this.selected();
    return r ? r.parameters.filter(p => p.required && this.form()[p.name]?.trim() === '').map(p => p.label || p.name) : [];
  });

  constructor() {
    // The designer saved or deleted something: the list changes.
    effect(() => {
      this.svc.revision();
      void this.reload();
    });
    // The reader clicked a legend, a heading or a group: lay the report out again with that.
    effect(() => {
      const view = this.view();
      untracked(() => void this.relayout(view));
    });
  }

  async ngOnInit(): Promise<void> {
    try {
      await this.svc.loadFonts();
    } catch (e) {
      this.error.set((e as Error).message);
    }
  }

  protected async reload(): Promise<void> {
    this.loading.set(true);
    try {
      const { reports, problems } = await this.svc.loadAll();
      this.reports.set(reports);
      this.problems.set(problems);
      const current = this.selected();
      if (current) {
        const again = reports.find(r => r.name === current.name);
        if (!again) this.selected.set(null);
        else if (JSON.stringify(again) !== JSON.stringify(current)) void this.select(again);
      }
    } catch (e) {
      if (!this.db.authRequired()) this.error.set((e as Error).message);
    } finally {
      this.loading.set(false);
    }
  }

  /** The reader chose a report in the list: a new start. */
  protected async choose(report: ReportDef): Promise<void> {
    this.trail.set([]);
    await this.select(report);
  }

  /** Opens a report with its default parameters, or with `given` (by name, as text) in their place. */
  protected async select(report: ReportDef, given?: Record<string, string>): Promise<void> {
    this.selected.set(report);
    this.layout.set(null);
    this.tables.set([]);
    this.warnings.set([]);
    this.error.set('');
    this.lastRun = null;
    this.view.set({});
    const form: Record<string, string> = {};
    for (const p of report.parameters) form[p.name] = given && p.name in given ? given[p.name] : paramText(p);
    this.form.set(form);
    this.choices.set({});
    for (const p of report.parameters) {
      if (!p.options?.length && !p.optionsSql) continue;
      try {
        const options = await this.svc.options(p, this.values(report));
        this.choices.update(c => ({ ...c, [p.name]: options }));
      } catch (e) {
        this.warnings.update(w => [...w, `the choices of ${p.label || p.name}: ${(e as Error).message}`]);
      }
    }
  }

  /** A click on a mark that opens another report: go there with the parameters it gives, and remember the way back. */
  protected async drill(request: DrillRequest): Promise<void> {
    const target = this.reports().find(r => r.name === request.report);
    const from = this.selected();
    if (!target) {
      this.notice.error(new Error(`There is no report called ${request.report}.`));
      return;
    }
    if (from) this.trail.update(t => [...t, { report: from, form: this.form() }]);
    await this.select(target, request.params);
    await this.run();
  }

  /** Back to the report `steps` levels up the trail, as it was run. */
  protected async back(steps = 1): Promise<void> {
    const trail = this.trail();
    const to = trail[trail.length - steps];
    if (!to) return;
    this.trail.set(trail.slice(0, trail.length - steps));
    await this.select(to.report, to.form);
    await this.run();
  }

  protected setValue(name: string, value: string): void {
    this.form.update(f => ({ ...f, [name]: value }));
  }

  /** The form as typed values: blank is null. */
  private values(report: ReportDef): Record<string, Value> {
    const out: Record<string, Value> = {};
    for (const p of report.parameters) out[p.name] = paramValue(p, this.form()[p.name] ?? '');
    return out;
  }

  protected async run(): Promise<void> {
    const report = this.selected();
    if (!report || this.running()) return;
    if (this.missing().length) {
      this.error.set(`Fill in ${this.missing().join(', ')}.`);
      return;
    }
    this.running.set(true);
    this.error.set('');
    try {
      this.lastValues = this.values(report);
      const result = await this.svc.run(report, this.lastValues);
      const laid = await this.svc.layout(report, this.lastValues, result.datasets, this.view(), result.subs);
      this.lastRun = result;
      this.layout.set(laid);
      this.warnings.set([...result.warnings, ...laid.warnings]);
      this.tables.set(tableData(report, this.lastValues, result.datasets, new Date(), this.view(), result.subs));
    } catch (e) {
      this.layout.set(null);
      this.tables.set([]);
      this.error.set((e as Error).message);
    } finally {
      this.running.set(false);
    }
  }

  /** The same data, laid out with what the reader changed. */
  private async relayout(view: ViewState): Promise<void> {
    const report = this.selected();
    if (!report || !this.lastRun) return;
    try {
      this.layout.set(await this.svc.layout(report, this.lastValues, this.lastRun.datasets, view, this.lastRun.subs));
      this.tables.set(tableData(report, this.lastValues, this.lastRun.datasets, new Date(), view, this.lastRun.subs));
    } catch (e) {
      this.error.set((e as Error).message);
    }
  }

  protected stop(): void {
    this.svc.interrupt();
  }

  protected async exportPdf(): Promise<void> {
    const report = this.selected();
    const laid = this.layout();
    if (!report || !laid) return;
    try {
      downloadBlob(`${report.name}.pdf`, await this.svc.pdf(laid, report));
    } catch (e) {
      this.notice.error(e);
    }
  }

  protected async exportXlsx(): Promise<void> {
    const report = this.selected();
    if (!report || !this.lastRun) return;
    try {
      downloadBlob(`${report.name}.xlsx`, await this.svc.xlsx(report, this.lastValues, this.lastRun, this.view()));
    } catch (e) {
      this.notice.error(e);
    }
  }

  protected async exportDocx(): Promise<void> {
    const report = this.selected();
    if (!report || !this.lastRun) return;
    try {
      downloadBlob(`${report.name}.docx`, await this.svc.docx(report, this.lastValues, this.lastRun, this.view()));
    } catch (e) {
      this.notice.error(e);
    }
  }

  protected exportCsv(table: TableData): void {
    const report = this.selected();
    if (report) downloadText(`${report.name}${this.tables().length > 1 ? ' - ' + table.title : ''}.csv`.replace(/[\\/:*?"<>|]/g, '_'), tableToCsv(table), 'text/csv;charset=utf-8');
  }

  protected optionsOf(p: ParamDef): ParamOption[] {
    return this.choices()[p.name] ?? p.options ?? [];
  }

  /** The choices of a parameter for its dropdown; one that is not required may also be left out. */
  protected choiceOptions(p: ParamDef): Choice[] {
    const options = this.optionsOf(p).map(o => ({ value: String(o.value), label: String(o.label ?? o.value) }));
    return p.required ? options : [{ value: '', label: '(any)' }, ...options];
  }

  protected hasChoices(p: ParamDef): boolean {
    return !!(p.options?.length || p.optionsSql);
  }
}
