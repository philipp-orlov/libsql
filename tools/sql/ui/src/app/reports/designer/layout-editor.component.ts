// Copyright (c) 2026 Philipp Orlov

import { NgTemplateOutlet } from '@angular/common';
import { Component, ElementRef, effect, inject, input, output, signal } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';

import { BandItem, BodyItem, ChartItem, ColumnDef, ReportDef, SeriesDef, TableItem } from '../core';
import { ItemPick } from '../viewer/report-viewer.component';
import { assign, Edit, move, numberOf } from './editing';
import { FIELD_MIME, FieldDrag, FieldInfo, KIND_ICON } from './fields';

export type Section = 'header' | 'body' | 'footer';
type Item = BodyItem | BandItem;

const ICONS: Record<string, string> = { text: 'title', table: 'table_chart', chart: 'bar_chart', rule: 'horizontal_rule', spacer: 'height', pageBreak: 'insert_page_break' };
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
  imports: [MatButtonModule, MatIconModule, NgTemplateOutlet],
  templateUrl: './layout-editor.component.html',
  styleUrl: './editor.scss',
})
export class LayoutEditorComponent {
  readonly report = input.required<ReportDef>();
  /** The fields of the datasets that have been run: offered as completions and by the shelves. */
  readonly fields = input<Record<string, FieldInfo[]>>({});
  /** The item to open, from a click in the preview. */
  readonly selection = input<ItemPick | null>(null);
  readonly edit = output<Edit>();
  /** The item whose form is open (the preview outlines it), or null. */
  readonly opened = output<ItemPick | null>();

  protected readonly sections: Section[] = ['body', 'header', 'footer'];
  protected readonly titles = SECTION_TITLES;
  protected readonly chartTypes = CHART_TYPES;
  protected readonly kindIcon = KIND_ICON;
  protected readonly open = signal<string | null>(null);
  protected readonly over = signal<string | null>(null);
  protected readonly message = signal('');
  private readonly host = inject<ElementRef<HTMLElement>>(ElementRef);

  constructor() {
    effect(() => {
      const s = this.selection();
      if (!s) return;
      this.open.set(this.key(s.section, s.index));
      setTimeout(() => this.host.nativeElement.querySelector('.card.active')?.scrollIntoView({ block: 'nearest', behavior: 'smooth' }));
    });
  }

  protected key(section: Section, i: number): string {
    return `${section}:${i}`;
  }

  protected toggle(section: Section, i: number): void {
    const next = this.open() === this.key(section, i) ? null : this.key(section, i);
    this.open.set(next);
    this.opened.emit(next ? { section, index: i } : null);
  }

  protected items(section: Section): Item[] {
    return this.report()[section];
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

  protected allowed(section: Section): string[] {
    return section === 'body' ? ['text', 'table', 'chart', 'rule', 'spacer', 'pageBreak'] : ['text', 'rule', 'spacer'];
  }

  protected label(type: string): string {
    return { text: 'Text', table: 'Table', chart: 'Chart', rule: 'Line', spacer: 'Space', pageBreak: 'Page break' }[type] ?? type;
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

  protected add(section: Section, type: string): void {
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
      case 'spacer': item = { type: 'spacer', height: 12 }; break;
      case 'pageBreak': item = { type: 'pageBreak' }; break;
      default: item = { type: 'rule' };
    }
    this.edit.emit(rep => (rep[section] as Item[]).push(item));
    const key = this.key(section, r[section].length);
    this.open.set(key);
    this.opened.emit({ section, index: r[section].length });
  }

  protected remove(section: Section, i: number): void {
    this.edit.emit(r => void r[section].splice(i, 1));
    this.open.set(null);
    this.opened.emit(null);
  }

  protected shift(section: Section, i: number, by: number): void {
    this.edit.emit(r => move(r[section], i, i + by));
    this.open.set(this.key(section, i + by));
    this.opened.emit({ section, index: i + by });
  }

  protected duplicate(section: Section, i: number): void {
    this.edit.emit(r => void (r[section] as Item[]).splice(i + 1, 0, structuredClone(r[section][i])));
    this.open.set(this.key(section, i + 1));
    this.opened.emit({ section, index: i + 1 });
  }

  protected set(section: Section, i: number, path: string, value: unknown): void {
    this.edit.emit(r => assign(r[section][i], path, value));
  }

  protected setNumber(section: Section, i: number, path: string, text: string): void {
    this.set(section, i, path, numberOf(text));
  }

  protected setDataset(section: Section, i: number, name: string): void {
    this.edit.emit(r => assign(r[section][i], 'dataset', name));
  }

  protected isType(item: ChartItem, t: ChartType): boolean {
    return item.chart === t.chart && !!item.stacked === !!t.flags.stacked && !!item.horizontal === !!t.flags.horizontal && !!item.donut === !!t.flags.donut;
  }

  protected setType(section: Section, i: number, t: ChartType): void {
    this.edit.emit(r => {
      const item = r[section][i] as ChartItem;
      item.chart = t.chart;
      delete item.stacked;
      delete item.horizontal;
      delete item.donut;
      Object.assign(item, t.flags);
    });
  }

  protected list(section: Section, i: number, name: 'columns' | 'groups' | 'series'): unknown[] {
    return (this.items(section)[i] as unknown as Record<string, unknown[]>)[name] ?? [];
  }

  protected addTo(section: Section, i: number, name: 'columns' | 'groups' | 'series', entry: unknown): void {
    this.edit.emit(r => {
      const item = r[section][i] as unknown as Record<string, unknown[]>;
      (item[name] ??= []).push(entry);
    });
  }

  protected newColumn(section: Section, i: number): ColumnDef {
    const item = this.items(section)[i] as TableItem;
    const used = new Set(item.columns.map(c => c.field));
    return { field: this.columnsOf(item.dataset).find(c => !used.has(c)) ?? '' };
  }

  protected addAllColumns(section: Section, i: number): void {
    const item = this.items(section)[i] as TableItem;
    const have = new Set(item.columns.map(c => c.field));
    const missing = this.columnsOf(item.dataset).filter(c => !have.has(c));
    this.edit.emit(r => (r[section][i] as TableItem).columns.push(...missing.map(field => ({ field }))));
  }

  protected newSeries(section: Section, i: number): SeriesDef {
    const item = this.items(section)[i] as ChartItem;
    const used = new Set([item.category, ...item.series.map(s => s.field)]);
    const numbers = this.fieldsOf(item.dataset).filter(f => f.kind === 'number' && !used.has(f.name));
    return { field: numbers[0]?.name ?? this.columnsOf(item.dataset).find(c => !used.has(c)) ?? '' };
  }

  protected removeFrom(section: Section, i: number, name: string, k: number): void {
    this.edit.emit(r => void (r[section][i] as unknown as Record<string, unknown[]>)[name].splice(k, 1));
  }

  protected moveIn(section: Section, i: number, name: string, k: number, by: number): void {
    this.edit.emit(r => move((r[section][i] as unknown as Record<string, unknown[]>)[name], k, k + by));
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
  protected drop(event: DragEvent, shelf: 'category' | 'series' | 'columns' | 'groups', section: Section, i: number): void {
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
    if (shelf === 'series' && f.kind !== 'number') {
      this.message.set(`A series is a number; ${f.field} is ${f.kind === 'date' ? 'a date' : 'text'}. Drop it on Category instead.`);
      return;
    }
    this.message.set('');
    this.edit.emit(r => {
      const it = r[section][i] as unknown as { dataset?: string; category?: string; series?: SeriesDef[]; columns: ColumnDef[]; groups?: { field: string; subtotals?: boolean }[] };
      if (!it.dataset) it.dataset = f.dataset;
      if (shelf === 'category') it.category = f.field;
      else if (shelf === 'series') (it.series ??= []).push({ field: f.field });
      else if (shelf === 'columns') it.columns.push({ field: f.field });
      else (it.groups ??= []).push({ field: f.field, subtotals: true });
    });
  }
}
