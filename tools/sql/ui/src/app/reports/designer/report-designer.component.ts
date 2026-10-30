// Copyright (c) 2026 Philipp Orlov

import { Component, OnDestroy, OnInit, computed, effect, inject, signal, untracked } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatCheckboxModule } from '@angular/material/checkbox';
import { MatDialog } from '@angular/material/dialog';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatIconModule } from '@angular/material/icon';
import { MatTabsModule } from '@angular/material/tabs';

import { ConfirmDialogComponent } from '../../components/confirm-dialog/confirm-dialog.component';
import { DatabaseService } from '../../services/database.service';
import { NoticeService } from '../../services/notice.service';
import { SplitComponent } from '../../shared/split-layout/split.component';
import { SplitPaneComponent } from '../../shared/split-layout/split-pane.component';
import { DataSet, forEachItem, isValidReportName, LayoutResult, newReport, PAGE_SIZES, PageSize, paramText, ParamOption, paramValue, parseReport, ReportDef, SubRuns, SubreportItem, validateReport, Value, ViewState } from '../core';
import { ReportService } from '../report.service';
import { DateComponent } from '../shared/date.component';
import { TextComponent } from '../shared/text.component';
import { Choice, SelectComponent } from '../shared/select.component';
import { ItemPick, ReportViewerComponent } from '../viewer/report-viewer.component';
import { DataPaneComponent } from './data-pane.component';
import { DatasetsEditorComponent } from './datasets-editor.component';
import { assign, Edit, numberOf } from './editing';
import { FieldInfo, fieldsOf } from './fields';
import { LayoutEditorComponent } from './layout-editor.component';
import { ParametersEditorComponent } from './parameters-editor.component';

/** What decides the data a report needs: its parameters and datasets, and the sub-reports it runs. */
function queriesOf(r: ReportDef): string {
  const subs: SubreportItem[] = [];
  forEachItem(r.body, it => it.type === 'subreport' && subs.push(it));
  return JSON.stringify([r.parameters, r.datasets, subs]);
}

const LAYOUT_TAB = 3;
const JSON_TAB = 4;
const HISTORY = 100;

/**
 * Designs a report: its page, parameters, datasets and layout. A data pane lists the fields to drag out, the preview follows
 * every change and is itself a way to edit (click an item to open its form), and every edit can be undone.
 */
@Component({
  selector: 'app-report-designer',
  imports: [
    MatButtonModule, MatCheckboxModule, MatFormFieldModule, MatIconModule, MatInputModule, MatTabsModule, SelectComponent, TextComponent, DateComponent, SplitComponent, SplitPaneComponent, DataPaneComponent, ParametersEditorComponent, DatasetsEditorComponent,
    LayoutEditorComponent, ReportViewerComponent,
  ],
  templateUrl: './report-designer.component.html',
  styleUrl: './report-designer.component.scss',
  host: { '(keydown)': 'onKey($event)' },
})
export class ReportDesignerComponent implements OnInit, OnDestroy {
  protected readonly svc = inject(ReportService);
  protected readonly db = inject(DatabaseService);
  private readonly notice = inject(NoticeService);
  private readonly dialog = inject(MatDialog);

  protected readonly names = signal<string[]>([]);
  protected readonly report = signal<ReportDef | null>(null);
  private readonly saved = signal('');
  /** The rows of the last run of the queries (the preview's data) and the first rows of the datasets run on their tab. */
  protected readonly data = signal<Record<string, DataSet>>({});
  /** The runs of the sub-reports the report holds (with the data). */
  private readonly subs = signal<SubRuns>({});
  private readonly samples = signal<Record<string, DataSet>>({});
  protected readonly layout = signal<LayoutResult | null>(null);
  protected readonly busy = signal(false);
  protected readonly error = signal('');
  protected readonly stale = signal(false);
  protected readonly tab = signal(0);
  protected readonly json = signal('');
  protected readonly jsonError = signal('');
  protected readonly newName = signal('');
  protected readonly copyName = signal('');
  protected readonly sizes = Object.keys(PAGE_SIZES) as PageSize[];
  protected readonly orientations: Choice[] = [{ value: 'portrait', label: 'Portrait' }, { value: 'landscape', label: 'Landscape' }];
  /** What the reader changed in the preview (legend, sort, folds). */
  protected readonly view = signal<ViewState>({});
  /** The item open in the layout editor, outlined in the preview. */
  protected readonly picked = signal<ItemPick | null>(null);
  /** The parameter fields above the preview, as text. */
  protected readonly form = signal<Record<string, string>>({});
  protected readonly choices = signal<Record<string, ParamOption[]>>({});
  private readonly past = signal<string[]>([]);
  private readonly future = signal<string[]>([]);

  protected readonly dirty = computed(() => {
    const r = this.report();
    return r !== null && JSON.stringify(r) !== this.saved();
  });
  protected readonly problems = computed(() => {
    const r = this.report();
    return r ? validateReport(r) : [];
  });
  protected readonly pages = computed(() => this.layout()?.pages ?? []);
  protected readonly canUndo = computed(() => this.past().length > 0);
  protected readonly canRedo = computed(() => this.future().length > 0);

  /** The fields of every dataset that has been run, with what kind of field each is. */
  protected readonly fields = computed<Record<string, FieldInfo[]>>(() => {
    const out: Record<string, FieldInfo[]> = {};
    for (const [name, ds] of Object.entries({ ...this.samples(), ...this.data() })) out[name] = fieldsOf(ds);
    return out;
  });
  protected readonly dataPane = computed(() =>
    (this.report()?.datasets ?? []).map(d => ({ name: d.name, fields: this.fields()[d.name] ?? [], sampled: d.name in this.fields() })),
  );

  private timer: ReturnType<typeof setTimeout> | undefined;
  private rerun: ReturnType<typeof setTimeout> | undefined;

  constructor() {
    // Lay the report out again shortly after the last change.
    effect(() => {
      const r = this.report();
      const data = this.data();
      const view = this.view();
      const form = this.form();
      const subs = this.subs();
      if (!r) return;
      untracked(() => this.schedule(r, data, view, form, subs));
    });
    effect(() => {
      const r = this.report();
      if (r && this.tab() === JSON_TAB) untracked(() => this.json.set(JSON.stringify(r, null, 2)));
    });
  }

  async ngOnInit(): Promise<void> {
    await this.refreshNames();
    try {
      await this.svc.loadFonts();
    } catch (e) {
      this.error.set((e as Error).message);
    }
  }

  ngOnDestroy(): void {
    clearTimeout(this.timer);
    clearTimeout(this.rerun);
  }

  private async refreshNames(): Promise<void> {
    try {
      this.names.set((await this.svc.list()).map(r => r.name));
    } catch (e) {
      if (!this.db.authRequired()) this.error.set((e as Error).message);
    }
  }

  private async confirm(title: string, message: string, action: string): Promise<boolean> {
    return !!(await this.dialog.open(ConfirmDialogComponent, { data: { title, message, action } }).afterClosed().toPromise());
  }

  private async discardOk(): Promise<boolean> {
    return !this.dirty() || this.confirm('Discard changes?', `${this.report()!.name} has changes that are not saved.`, 'Discard');
  }

  private adopt(r: ReportDef): void {
    this.report.set(r);
    this.saved.set(JSON.stringify(r));
    this.data.set({});
    this.subs.set({});
    this.samples.set({});
    this.layout.set(null);
    this.error.set('');
    this.stale.set(r.datasets.length > 0);
    this.tab.set(0);
    this.past.set([]);
    this.future.set([]);
    this.view.set({});
    this.picked.set(null);
    this.form.set(Object.fromEntries(r.parameters.map(p => [p.name, paramText(p)])));
    this.choices.set({});
    void this.loadChoices(r);
  }

  // ---- files ---------------------------------------------------------------------------------------------------------

  protected async create(): Promise<void> {
    const name = this.newName().trim();
    if (!isValidReportName(name)) return void this.notice.error(new Error('A report name is 1 to 80 letters, digits, spaces, dashes, underscores or dots.'));
    if (this.names().includes(name)) return void this.notice.error(new Error(`There is a report called ${name} already.`));
    if (!(await this.discardOk())) return;
    this.adopt(newReport(name));
    this.newName.set('');
    this.saved.set(''); // not on disk yet
  }

  protected async open(name: string): Promise<void> {
    if (!name || name === this.report()?.name || !(await this.discardOk())) return;
    try {
      this.adopt({ ...(await this.svc.load(name)), name });
      void this.refreshData();
    } catch (e) {
      this.notice.error(e);
    }
  }

  protected async save(asName?: string): Promise<void> {
    const r = this.report();
    if (!r) return;
    const out = asName ? { ...r, name: asName } : r;
    if (!isValidReportName(out.name)) return void this.notice.error(new Error('Give the report a valid file name on the General tab.'));
    try {
      await this.svc.save(out);
      this.report.set(out);
      this.saved.set(JSON.stringify(out));
      await this.refreshNames();
      this.notice.info(`Saved ${out.name}.`);
    } catch (e) {
      this.notice.error(e);
    }
  }

  protected async saveAs(name: string): Promise<void> {
    name = name.trim();
    if (!name) return;
    if (this.names().includes(name) && !(await this.confirm('Replace report?', `${name} exists and will be overwritten.`, 'Replace'))) return;
    await this.save(name);
  }

  protected async remove(): Promise<void> {
    const r = this.report();
    if (!r) return;
    if (!(await this.confirm('Delete report?', `${r.name} will be deleted from the server.`, 'Delete'))) return;
    try {
      if (this.names().includes(r.name)) await this.svc.remove(r.name);
      this.report.set(null);
      this.saved.set('');
      this.layout.set(null);
      await this.refreshNames();
    } catch (e) {
      this.notice.error(e);
    }
  }

  // ---- editing, undo and redo ----------------------------------------------------------------------------------------

  protected apply(edit: Edit): void {
    const before = this.report()!;
    const r = structuredClone(before);
    const queries = queriesOf(r);
    edit(r);
    if (JSON.stringify(r) === JSON.stringify(before)) return;
    this.past.update(p => [...p.slice(-(HISTORY - 1)), JSON.stringify(before)]);
    this.future.set([]);
    this.set(r, queries !== queriesOf(r));
  }

  private set(r: ReportDef, queriesChanged: boolean): void {
    if (queriesChanged) this.stale.set(true);
    this.report.set(r);
    this.syncForm(r);
    if (queriesChanged) this.scheduleRun();
  }

  protected undo(): void {
    const past = this.past();
    if (!past.length) return;
    this.future.update(f => [...f, JSON.stringify(this.report())]);
    this.past.set(past.slice(0, -1));
    this.set(JSON.parse(past[past.length - 1]) as ReportDef, true);
  }

  protected redo(): void {
    const future = this.future();
    if (!future.length) return;
    this.past.update(p => [...p, JSON.stringify(this.report())]);
    this.future.set(future.slice(0, -1));
    this.set(JSON.parse(future[future.length - 1]) as ReportDef, true);
  }

  protected onKey(event: KeyboardEvent): void {
    const mod = event.ctrlKey || event.metaKey;
    if (!mod || !this.report()) return;
    const typing = event.target instanceof HTMLTextAreaElement || (event.target instanceof HTMLInputElement && !['checkbox', 'color', 'range'].includes(event.target.type)) || (event.target as HTMLElement).isContentEditable;
    const key = event.key.toLowerCase();
    if (key === 's') {
      event.preventDefault();
      event.stopPropagation(); // not the shell's "write changes"
      void this.save();
    } else if (!typing && key === 'z') {
      event.preventDefault();
      if (event.shiftKey) this.redo();
      else this.undo();
    } else if (!typing && key === 'y') {
      event.preventDefault();
      this.redo();
    }
  }

  protected setField(path: string, value: unknown): void {
    this.apply(r => assign(r, path, value));
  }

  protected setMargin(side: string, text: string): void {
    const n = numberOf(text);
    if (n !== undefined) this.apply(r => (r.page.margins[side as 'top'] = n));
  }

  protected applyJson(text: string): void {
    this.json.set(text);
    try {
      const r = parseReport(text);
      this.jsonError.set('');
      this.apply(rep => Object.assign(rep, r));
    } catch (e) {
      this.jsonError.set((e as Error).message);
    }
  }

  /** A dataset was run on its tab: remember its first rows, for the data pane. */
  protected ran(event: { name: string; data: DataSet }): void {
    this.samples.update(s => ({ ...s, [event.name]: event.data }));
  }

  // ---- the preview as an editor ---------------------------------------------------------------------------------------

  /** A click on an item of the preview opens its form. */
  protected pick(item: ItemPick): void {
    this.picked.set(item);
    this.tab.set(LAYOUT_TAB);
  }

  // ---- preview data -----------------------------------------------------------------------------------------------------

  private syncForm(r: ReportDef): void {
    const form = { ...this.form() };
    let changed = false;
    for (const p of r.parameters) if (!(p.name in form)) ((form[p.name] = paramText(p)), (changed = true));
    for (const name of Object.keys(form)) if (!r.parameters.some(p => p.name === name)) (delete form[name], (changed = true));
    if (changed) this.form.set(form);
  }

  private async loadChoices(r: ReportDef): Promise<void> {
    for (const p of r.parameters) {
      if (!p.options?.length && !p.optionsSql) continue;
      try {
        const options = await this.svc.options(p, this.values(r));
        this.choices.update(c => ({ ...c, [p.name]: options }));
      } catch {
        // the choices are a convenience here: the field still takes a typed value
      }
    }
  }

  protected setValue(name: string, value: string): void {
    this.form.update(f => ({ ...f, [name]: value }));
    this.scheduleRun();
  }

  /** The parameter fields as the typed values the queries bind. */
  private values(r: ReportDef): Record<string, Value> {
    const out: Record<string, Value> = {};
    for (const p of r.parameters) out[p.name] = paramValue(p, this.form()[p.name] ?? paramText(p));
    return out;
  }

  /** Run the queries soon after the last change to a query or a parameter, so that the preview stays with them. */
  private scheduleRun(): void {
    clearTimeout(this.rerun);
    this.rerun = setTimeout(() => void this.refreshData(), 700);
  }

  protected async refreshData(): Promise<void> {
    const r = this.report();
    if (!r || this.busy()) return;
    if (!r.datasets.length) return void (this.data.set({}), this.subs.set({}));
    this.busy.set(true);
    this.error.set('');
    try {
      const result = await this.svc.run(r, this.values(r));
      this.data.set(result.datasets);
      this.subs.set(result.subs);
      this.stale.set(false);
    } catch (e) {
      this.error.set((e as Error).message);
    } finally {
      this.busy.set(false);
    }
  }

  protected stop(): void {
    this.svc.interrupt();
  }

  /** The choices of a parameter's dropdown; one that is not required may also be left out. */
  protected choiceOptions(p: { name: string; required?: boolean }): Choice[] {
    const options = this.choicesOf(p.name).map(o => ({ value: String(o.value), label: String(o.label ?? o.value) }));
    return p.required ? options : [{ value: '', label: '(any)' }, ...options];
  }

  protected choicesOf(name: string): ParamOption[] {
    return this.choices()[name] ?? [];
  }

  private schedule(r: ReportDef, data: Record<string, DataSet>, view: ViewState, form: Record<string, string>, subs: SubRuns): void {
    clearTimeout(this.timer);
    this.timer = setTimeout(async () => {
      try {
        const params: Record<string, Value> = {};
        for (const p of r.parameters) params[p.name] = paramValue(p, form[p.name] ?? paramText(p));
        this.layout.set(await this.svc.layout(r, params, data, view, subs));
      } catch (e) {
        this.error.set((e as Error).message);
      }
    }, 150);
  }
}
