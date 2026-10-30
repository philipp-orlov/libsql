// Copyright (c) 2026 Philipp Orlov

import { NgTemplateOutlet } from '@angular/common';
import { Component, ElementRef, effect, inject, input, output, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';

import { BandItem, BodyItem, ChartItem, ColumnDef, ColumnsItem, DrillDef, ImageItem, MatrixItem, ReportDef, SeriesDef, SubreportItem, TableItem } from '../core';
import { ComboComponent } from '../shared/combo.component';
import { MatCheckboxModule } from '@angular/material/checkbox';
import { TextComponent } from '../shared/text.component';
import { Choice, SelectComponent } from '../shared/select.component';
import { ItemPick } from '../viewer/report-viewer.component';
import { assign, Edit, listAt, move, numberOf } from './editing';
import { FIELD_MIME, FieldDrag, FieldInfo, KIND_ICON } from './fields';

export type Section = 'header' | 'body' | 'footer';
type Item = BodyItem | BandItem;
/** Where a list of items is: `body`, `header`, `footer`, or `body.2.columns.0.items` for the items inside a column. */
type ListPath = string;

const ICONS: Record<string, string> = {
  text: 'title', table: 'table_chart', chart: 'bar_chart', matrix: 'grid_view', image: 'image', columns: 'view_column', subreport: 'dynamic_feed',
  rule: 'horizontal_rule', spacer: 'height', pageBreak: 'insert_page_break',
};
const HEATS = ['blue', 'green', 'orange', 'red', 'purple', 'teal', 'diverging', 'none'];
const MAX_IMAGE = 700 * 1024;
const SECTION_TITLES: Record<Section, string> = { header: 'Page header · every page', body: 'Body', footer: 'Page footer · every page' };

/** What the gallery offers: a chart kind and the flags that make it. */
export interface ChartType {
  id: string;
  icon: string;
  label: string;
  chart: ChartItem['chart'];
  flags: Partial<Pick<ChartItem, 'stacked' | 'horizontal' | 'donut'>>;
}
export const CHART_TYPES: ChartType[] = [
  { id: 'bar', icon: 'bar_chart', label: 'Bars', chart: 'bar', flags: {} },
  { id: 'stacked', icon: 'stacked_bar_chart', label: 'Stacked', chart: 'bar', flags: { stacked: true } },
  { id: 'horizontal', icon: 'align_horizontal_left', label: 'Horizontal', chart: 'bar', flags: { horizontal: true } },
  { id: 'line', icon: 'show_chart', label: 'Lines', chart: 'line', flags: {} },
  { id: 'area', icon: 'area_chart', label: 'Areas', chart: 'area', flags: {} },
  { id: 'pie', icon: 'pie_chart', label: 'Pie', chart: 'pie', flags: {} },
  { id: 'donut', icon: 'donut_large', label: 'Donut', chart: 'pie', flags: { donut: true } },
];

/** The page header, body and page footer of a report: a list of items, each with a form for its properties. */
@Component({
  selector: 'app-layout-editor',
  imports: [MatButtonModule, MatIconModule, NgTemplateOutlet, SelectComponent, ComboComponent, TextComponent, MatCheckboxModule],
  templateUrl: './layout-editor.component.html',
  styleUrl: './editor.scss',
})
export class LayoutEditorComponent {
  readonly report = input.required<ReportDef>();
  /** The fields of the datasets that have been run: offered as completions and by the shelves. */
  readonly fields = input<Record<string, FieldInfo[]>>({});
  /** The item to open, from a click in the preview. */
  readonly selection = input<ItemPick | null>(null);
  /** The reports on the server: what a drill-through or a sub-report can open. */
  readonly reportNames = input<string[]>([]);
  readonly edit = output<Edit>();
  /** The item whose form is open (the preview outlines it), or null. */
  readonly opened = output<ItemPick | null>();

  protected readonly sections: Section[] = ['body', 'header', 'footer'];
  protected readonly titles = SECTION_TITLES;
  protected readonly chartTypes = CHART_TYPES;
  protected readonly faceOptions: Choice[] = [{ value: 'regular', label: 'Regular' }, { value: 'bold', label: 'Bold' }];
  protected readonly alignOptions: Choice[] = [{ value: '', label: 'Default' }, { value: 'left', label: 'Left' }, { value: 'center', label: 'Center' }, { value: 'right', label: 'Right' }];
  protected readonly autoAlignOptions: Choice[] = [{ value: '', label: 'Auto' }, { value: 'left', label: 'Left' }, { value: 'center', label: 'Center' }, { value: 'right', label: 'Right' }];
  protected readonly heatOptions: Choice[] = HEATS.map(h => ({ value: h === 'blue' ? '' : h, label: h === 'blue' ? 'blue (default)' : h }));
  protected readonly aggOptions: Choice[] = [{ value: '', label: 'sum (default)' }, ...['avg', 'min', 'max', 'count'].map(t => ({ value: t, label: t }))];
  protected readonly orderMatrixRows: Choice[] = [{ value: '', label: 'As the data comes' }, { value: 'asc', label: 'Ascending' }, { value: 'desc', label: 'Descending' }, { value: 'total', label: 'Largest total first' }];
  protected readonly orderMatrixColumns: Choice[] = [{ value: '', label: 'As the data comes' }, { value: 'asc', label: 'Ascending' }, { value: 'desc', label: 'Descending' }];
  protected readonly imageAlign: Choice[] = [{ value: '', label: 'Left' }, { value: 'center', label: 'Center' }, { value: 'right', label: 'Right' }];
  protected readonly orderOptions: Choice[] = [{ value: '', label: 'Ascending' }, { value: 'desc', label: 'Descending' }];
  protected readonly totalOptions: Choice[] = [{ value: '', label: 'None' }, ...['sum', 'avg', 'min', 'max', 'count'].map(t => ({ value: t, label: t }))];
  protected readonly kindIcon = KIND_ICON;
  protected readonly open = signal<string | null>(null);
  protected readonly over = signal<string | null>(null);
  protected readonly message = signal('');
  private readonly host = inject<ElementRef<HTMLElement>>(ElementRef);

  constructor() {
    effect(() => {
      const s = this.selection();
      if (!s) return;
      this.open.set(this.key(s.list ?? s.section, s.index));
      setTimeout(() => this.host.nativeElement.querySelector('.card.active')?.scrollIntoView({ block: 'nearest', behavior: 'smooth' }));
    });
  }

  protected key(section: ListPath, i: number): string {
    return `${section}:${i}`;
  }

  /** The item of the report the preview should outline for item `i` of the list: for one inside columns, the columns item. */
  private pick(section: ListPath, i: number): ItemPick {
    return section === 'body' || section === 'header' || section === 'footer' ? { section, index: i } : { section: 'body', index: i, list: section };
  }

  protected toggle(section: ListPath, i: number): void {
    const next = this.open() === this.key(section, i) ? null : this.key(section, i);
    this.open.set(next);
    this.opened.emit(next ? this.pick(section, i) : null);
  }

  protected items(section: ListPath): Item[] {
    return listAt(this.report(), section) as Item[];
  }

  /** Whether the item's form is showing: it is the open one, or holds the open one. */
  protected isOpen(section: ListPath, i: number): boolean {
    const o = this.open();
    return o === this.key(section, i) || (o !== null && o.startsWith(`${section}.${i}.`));
  }

  /** How deep in columns a list is: 0 for the page's own lists. */
  protected depth(section: ListPath): number {
    return section.split('.').length > 1 ? section.split('.').filter(p => p === 'columns').length : 0;
  }

  protected icon(item: Item): string {
    return ICONS[item.type] ?? 'help';
  }

  protected iconFor(type: string): string {
    return ICONS[type] ?? 'help';
  }

  protected summary(item: Item): string {
    switch (item.type) {
      case 'text': return item.text.replace(/\s+/g, ' ').slice(0, 70) || '(empty text)';
      case 'matrix': return `Cross-tab of ${item.dataset || '?'}: ${item.value || '?'} by ${item.rows.join(' / ') || '?'} and ${item.column || '?'}`;
      case 'image': return item.caption ? `Picture: ${item.caption}` : item.src ? 'Picture' : 'Picture (none chosen yet)';
      case 'columns': return `${item.columns.length} columns side by side`;
      case 'subreport': return `Sub-report ${item.report || '?'}${item.dataset ? ` for each row of ${item.dataset}` : ''}`;
      case 'table': return `Table of ${item.dataset || '?'}, ${item.columns.length} column${item.columns.length === 1 ? '' : 's'}${item.groups?.length ? `, grouped by ${item.groups.map(g => g.field).join(', ')}` : ''}`;
      case 'chart': return `${this.chartLabel(item)} of ${item.dataset || '?'}${item.title ? ': ' + item.title : ''}`;
      case 'rule': return 'Line';
      case 'spacer': return `Space of ${item.height} pt`;
      default: return 'Page break';
    }
  }

  private chartLabel(item: ChartItem): string {
    return (this.chartTypes.find(t => this.isType(item, t)) ?? this.chartTypes[0]).label + ' chart';
  }

  protected allowed(section: ListPath): string[] {
    if (section === 'header' || section === 'footer') return ['text', 'image', 'rule', 'spacer'];
    const inner = ['text', 'table', 'chart', 'matrix', 'image', 'subreport', 'rule', 'spacer'];
    return section === 'body' ? ['text', 'table', 'chart', 'matrix', 'image', 'columns', 'subreport', 'rule', 'spacer', 'pageBreak'] : inner;
  }

  protected label(type: string): string {
    return { text: 'Text', table: 'Table', chart: 'Chart', matrix: 'Cross-tab', image: 'Picture', columns: 'Columns', subreport: 'Sub-report', rule: 'Line', spacer: 'Space', pageBreak: 'Page break' }[type] ?? type;
  }

  protected datasetNames(): string[] {
    return this.report().datasets.map(d => d.name);
  }

  protected fieldsOf(dataset: string | undefined): FieldInfo[] {
    return (dataset && this.fields()[dataset]) || [];
  }

  protected columnsOf(dataset: string | undefined): string[] {
    return this.fieldsOf(dataset).map(f => f.name);
  }

  // ---- changes --------------------------------------------------------------------------------------------------------

  protected add(section: ListPath, type: string): void {
    const r = this.report();
    const dataset = r.datasets[0]?.name ?? '';
    const fields = this.fieldsOf(dataset);
    const dims = fields.filter(f => f.kind !== 'number');
    const measures = fields.filter(f => f.kind === 'number');
    let item: Item;
    switch (type) {
      case 'text': item = { type: 'text', text: 'Text' }; break;
      case 'table': item = { type: 'table', dataset, columns: fields.slice(0, 8).map(f => ({ field: f.name })), zebra: true }; break;
      case 'chart': item = { type: 'chart', chart: 'bar', dataset, category: (dims[0] ?? fields[0])?.name ?? '', series: measures[0] ? [{ field: measures[0].name }] : [], height: 220 }; break;
      case 'matrix': item = { type: 'matrix', dataset, rows: [(dims[0] ?? fields[0])?.name ?? ''], column: (dims[1] ?? dims[0] ?? fields[0])?.name ?? '', value: measures[0]?.name ?? '', rowTotals: true, columnTotals: true, format: '#,##0' }; break;
      case 'image': item = { type: 'image', src: '' }; break;
      case 'columns': item = { type: 'columns', columns: [{ items: [] }, { items: [] }] }; break;
      case 'subreport': item = { type: 'subreport', report: this.reportNames().find(n => n !== r.name) ?? '' }; break;
      case 'spacer': item = { type: 'spacer', height: 12 }; break;
      case 'pageBreak': item = { type: 'pageBreak' }; break;
      default: item = { type: 'rule' };
    }
    const at = listAt(r, section).length;
    this.edit.emit(rep => (listAt(rep, section) as Item[]).push(item));
    this.open.set(this.key(section, at));
    this.opened.emit(this.pick(section, at));
  }

  protected remove(section: ListPath, i: number): void {
    this.edit.emit(r => void listAt(r, section).splice(i, 1));
    this.open.set(null);
    this.opened.emit(null);
  }

  protected shift(section: ListPath, i: number, by: number): void {
    this.edit.emit(r => move(listAt(r, section), i, i + by));
    this.open.set(this.key(section, i + by));
    this.opened.emit(this.pick(section, i + by));
  }

  protected duplicate(section: ListPath, i: number): void {
    this.edit.emit(r => void listAt(r, section).splice(i + 1, 0, structuredClone(listAt(r, section)[i])));
    this.open.set(this.key(section, i + 1));
    this.opened.emit(this.pick(section, i + 1));
  }

  protected set(section: ListPath, i: number, path: string, value: unknown): void {
    this.edit.emit(r => assign(listAt(r, section)[i], path, value));
  }

  protected setNumber(section: ListPath, i: number, path: string, text: string): void {
    this.set(section, i, path, numberOf(text));
  }

  protected setDataset(section: ListPath, i: number, name: string): void {
    this.edit.emit(r => assign(listAt(r, section)[i], 'dataset', name));
  }

  protected isType(item: ChartItem, t: ChartType): boolean {
    return item.chart === t.chart && !!item.stacked === !!t.flags.stacked && !!item.horizontal === !!t.flags.horizontal && !!item.donut === !!t.flags.donut;
  }

  protected setType(section: ListPath, i: number, t: ChartType): void {
    this.edit.emit(r => {
      const item = listAt(r, section)[i] as ChartItem;
      item.chart = t.chart;
      delete item.stacked;
      delete item.horizontal;
      delete item.donut;
      Object.assign(item, t.flags);
    });
  }

  protected list(section: ListPath, i: number, name: 'columns' | 'groups' | 'series'): unknown[] {
    return (this.items(section)[i] as unknown as Record<string, unknown[]>)[name] ?? [];
  }

  protected addTo(section: ListPath, i: number, name: 'columns' | 'groups' | 'series', entry: unknown): void {
    this.edit.emit(r => {
      const item = listAt(r, section)[i] as unknown as Record<string, unknown[]>;
      (item[name] ??= []).push(entry);
    });
  }

  protected newColumn(section: ListPath, i: number): ColumnDef {
    const item = this.items(section)[i] as TableItem;
    const used = new Set(item.columns.map(c => c.field));
    return { field: this.columnsOf(item.dataset).find(c => !used.has(c)) ?? '' };
  }

  protected addAllColumns(section: ListPath, i: number): void {
    const item = this.items(section)[i] as TableItem;
    const have = new Set(item.columns.map(c => c.field));
    const missing = this.columnsOf(item.dataset).filter(c => !have.has(c));
    this.edit.emit(r => (listAt(r, section)[i] as TableItem).columns.push(...missing.map(field => ({ field }))));
  }

  protected newSeries(section: ListPath, i: number): SeriesDef {
    const item = this.items(section)[i] as ChartItem;
    const used = new Set([item.category, ...item.series.map(s => s.field)]);
    const numbers = this.fieldsOf(item.dataset).filter(f => f.kind === 'number' && !used.has(f.name));
    return { field: numbers[0]?.name ?? this.columnsOf(item.dataset).find(c => !used.has(c)) ?? '' };
  }

  protected removeFrom(section: ListPath, i: number, name: string, k: number): void {
    this.edit.emit(r => void (listAt(r, section)[i] as unknown as Record<string, unknown[]>)[name].splice(k, 1));
  }

  protected moveIn(section: ListPath, i: number, name: string, k: number, by: number): void {
    this.edit.emit(r => move((listAt(r, section)[i] as unknown as Record<string, unknown[]>)[name], k, k + by));
  }

  protected asMatrix(item: Item): MatrixItem {
    return item as MatrixItem;
  }

  protected asImage(item: Item): ImageItem {
    return item as ImageItem;
  }

  protected asColumns(item: Item): ColumnsItem {
    return item as ColumnsItem;
  }

  protected asSub(item: Item): SubreportItem {
    return item as SubreportItem;
  }

  protected asRich(item: Item): { rich?: boolean } {
    return item as never;
  }

  protected drillOf(item: Item): DrillDef | undefined {
    return (item as { drill?: DrillDef }).drill;
  }

  protected paramKeys(map: Record<string, string> | undefined): string[] {
    return Object.keys(map ?? {});
  }

  // ---- drill-through and sub-report parameters --------------------------------------------------------------------------
  // The parameters are a map, name to a text with {{ }} parts; `holder` says where it is: on the item's `drill`, or the item itself.

  private paramsOf(r: ReportDef, section: ListPath, i: number, holder: 'drill' | 'self'): Record<string, string> {
    const item = listAt(r, section)[i] as { drill?: DrillDef; params?: Record<string, string> };
    if (holder === 'drill') return ((item.drill ??= { report: '' }).params ??= {});
    return (item.params ??= {});
  }

  protected setDrillReport(section: ListPath, i: number, report: string): void {
    this.edit.emit(r => {
      const item = listAt(r, section)[i] as { drill?: DrillDef };
      if (!report) delete item.drill;
      else (item.drill ??= { report }).report = report;
    });
  }

  protected setParam(section: ListPath, i: number, holder: 'drill' | 'self', key: string, value: string): void {
    this.edit.emit(r => (this.paramsOf(r, section, i, holder)[key] = value));
  }

  protected renameParam(section: ListPath, i: number, holder: 'drill' | 'self', from: string, to: string): void {
    this.edit.emit(r => {
      const map = this.paramsOf(r, section, i, holder);
      if (!to || to === from || to in map) return;
      // keep the order: rebuild the map
      const entries = Object.entries(map).map(([k, v]) => [k === from ? to : k, v] as const);
      for (const k of Object.keys(map)) delete map[k];
      for (const [k, v] of entries) map[k] = v;
    });
  }

  protected removeParam(section: ListPath, i: number, holder: 'drill' | 'self', key: string): void {
    this.edit.emit(r => {
      const map = this.paramsOf(r, section, i, holder);
      delete map[key];
    });
  }

  protected addParam(section: ListPath, i: number, holder: 'drill' | 'self'): void {
    this.edit.emit(r => {
      const map = this.paramsOf(r, section, i, holder);
      let n = 1;
      while (`param${n}` in map) n++;
      map[`param${n}`] = '';
    });
  }

  // ---- pictures ------------------------------------------------------------------------------------------------------------

  protected upload(event: Event, section: ListPath, i: number): void {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    input.value = '';
    if (!file) return;
    if (file.size > MAX_IMAGE) {
      this.message.set(`${file.name} is ${Math.round(file.size / 1024)} KiB; a picture kept in a report can be 700 KiB at most. Shrink it first.`);
      return;
    }
    if (!/^image\/(png|jpeg|gif|webp|svg\+xml)$/.test(file.type)) {
      this.message.set(`${file.name} is not a PNG, JPEG, GIF, WebP or SVG picture.`);
      return;
    }
    const reader = new FileReader();
    reader.onload = () => {
      this.message.set('');
      this.set(section, i, 'src', String(reader.result));
    };
    reader.readAsDataURL(file);
  }

  // ---- rich text ----------------------------------------------------------------------------------------------------------------

  /** Puts `[tag]` ... `[/tag]` around what is selected in the text box of the open item (the edit applies when the box is left, so it is read from the page). */
  protected markup(section: ListPath, i: number, open: string, close: string): void {
    const box = this.host.nativeElement.querySelector<HTMLTextAreaElement>('.card.active textarea');
    const item = this.items(section)[i] as { text?: string };
    if (!box || item.text === undefined) return;
    const text = box.value;
    const from = box.selectionStart ?? text.length;
    const to = box.selectionEnd ?? text.length;
    this.set(section, i, 'text', text.slice(0, from) + open + (text.slice(from, to) || 'text') + close + text.slice(to));
  }

  protected asTable(item: Item): TableItem {
    return item as TableItem;
  }

  protected asChart(item: Item): ChartItem {
    return item as ChartItem;
  }

  protected asText(item: Item): { text: string; dataset?: string; style?: Record<string, unknown> } {
    return item as never;
  }

  // ---- fields dragged from the data pane --------------------------------------------------------------------------------

  protected dragOver(event: DragEvent, zone: string): void {
    if (!event.dataTransfer?.types.includes(FIELD_MIME)) return;
    event.preventDefault();
    event.dataTransfer.dropEffect = 'copy';
    this.over.set(zone);
  }

  protected dragLeave(zone: string): void {
    if (this.over() === zone) this.over.set(null);
  }

  /** A field dropped on a shelf of the open item: the category, a series, a table column or a group. */
  protected drop(event: DragEvent, shelf: 'category' | 'series' | 'columns' | 'groups' | 'rows' | 'column' | 'value', section: ListPath, i: number): void {
    this.over.set(null);
    const raw = event.dataTransfer?.getData(FIELD_MIME);
    if (!raw) return;
    event.preventDefault();
    const f = JSON.parse(raw) as FieldDrag;
    const item = this.items(section)[i] as TableItem | ChartItem;
    if (item.dataset && item.dataset !== f.dataset) {
      this.message.set(`This item shows ${item.dataset}; ${f.field} belongs to ${f.dataset}. Change the item's dataset first.`);
      return;
    }
    if ((shelf === 'series' || shelf === 'value') && f.kind !== 'number') {
      this.message.set(`A series is a number; ${f.field} is ${f.kind === 'date' ? 'a date' : 'text'}. Drop it on ${shelf === 'value' ? 'Rows or Columns' : 'Category'} instead.`);
      return;
    }
    this.message.set('');
    this.edit.emit(r => {
      const it = listAt(r, section)[i] as unknown as { dataset?: string; category?: string; series?: SeriesDef[]; columns: ColumnDef[]; groups?: { field: string; subtotals?: boolean }[]; rows?: string[]; column?: string; value?: string };
      if (!it.dataset) it.dataset = f.dataset;
      if (shelf === 'rows') {
        const rows = (it.rows ??= []);
        if (rows.length < 2) rows.push(f.field);
        else rows[1] = f.field;
      } else if (shelf === 'column') it.column = f.field;
      else if (shelf === 'value') it.value = f.field;
      else if (shelf === 'category') it.category = f.field;
      else if (shelf === 'series') (it.series ??= []).push({ field: f.field });
      else if (shelf === 'columns') it.columns.push({ field: f.field });
      else (it.groups ??= []).push({ field: f.field, subtotals: true });
    });
  }
}
