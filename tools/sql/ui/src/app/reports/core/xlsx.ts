// Copyright (c) 2026 Philipp Orlov

// A report as an Excel workbook (.xlsx, written by hand): a first sheet that reads like the top of the report (title,
// parameters, the texts) and a sheet for every table, cross-tab and chart, holding real numbers with their number formats,
// styled headings, group and total rows, the cross-tab's colors, frozen headings and filters. Free of Angular imports on purpose.

import { Block, walkBlocks } from './blocks';
import { ReportDef } from './definition';
import { formatValue, toDate, Value } from './format';
import { heatFill, inkOn } from './matrix';
import { plainText } from './richtext';
import { TableRow } from './tablemodel';
import { buildZip } from './zip';

export interface WorkbookInfo {
  report: ReportDef;
  params: Record<string, Value>;
  blocks: Block[];
  now?: Date;
}

const XML = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n';
const NS = 'http://schemas.openxmlformats.org/spreadsheetml/2006/main';

const esc = (s: string): string => s.replace(/[\u0000-\u0008\u000B\u000C\u000E-\u001F]/g, '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

/** A report's number or date pattern as an Excel format code. */
export function excelFormat(pattern: string | undefined): string {
  if (!pattern) return 'General';
  if (/yyyy|yy|MM|dd|HH|mm|ss/.test(pattern)) return pattern.replace(/MM/g, 'mm').replace(/HH/g, 'hh');
  // text around the digits has to be quoted; the digit marks, separators and signs are format characters
  return pattern.replace(/[A-Za-z\u0080-￿][^#0.,%$\-+()\s]*/g, t => `"${t.replace(/"/g, '')}"`);
}

const col = (n: number): string => {
  let s = '';
  for (let i = n + 1; i > 0; i = Math.floor((i - 1) / 26)) s = String.fromCharCode(65 + ((i - 1) % 26)) + s;
  return s;
};

interface Look {
  bold?: boolean;
  italic?: boolean;
  size?: number;
  color?: string;
  fill?: string;
  fmt?: string;
  align?: 'left' | 'center' | 'right';
  indent?: number;
  wrap?: boolean;
  top?: 'thin' | 'medium';
  bottom?: 'thin' | 'medium';
  underline?: boolean;
}

class Styles {
  private readonly fonts: string[] = [];
  private readonly fills: string[] = ['<fill><patternFill patternType="none"/></fill>', '<fill><patternFill patternType="gray125"/></fill>'];
  private readonly borders: string[] = ['<border><left/><right/><top/><bottom/><diagonal/></border>'];
  private readonly numFmts: string[] = [];
  private readonly xfs: string[] = [];
  private readonly index = new Map<string, number>();
  constructor() {
    this.style({});
  }
  private pick(list: string[], xml: string): number {
    const at = list.indexOf(xml);
    if (at >= 0) return at;
    list.push(xml);
    return list.length - 1;
  }
  style(l: Look): number {
    const key = JSON.stringify(l);
    const have = this.index.get(key);
    if (have !== undefined) return have;
    const font = this.pick(
      this.fonts,
      `<font>${l.bold ? '<b/>' : ''}${l.italic ? '<i/>' : ''}${l.underline ? '<u/>' : ''}<sz val="${l.size ?? 10}"/><color rgb="FF${(l.color ?? '#222222').slice(1).toUpperCase()}"/><name val="Arial"/><family val="2"/></font>`,
    );
    const fill = l.fill ? this.pick(this.fills, `<fill><patternFill patternType="solid"><fgColor rgb="FF${l.fill.slice(1).toUpperCase()}"/><bgColor indexed="64"/></patternFill></fill>`) : 0;
    const side = (name: string, kind?: string): string => (kind ? `<${name} style="${kind}"><color rgb="FF6B7280"/></${name}>` : `<${name}/>`);
    const border = l.top || l.bottom ? this.pick(this.borders, `<border><left/><right/>${side('top', l.top)}${side('bottom', l.bottom)}<diagonal/></border>`) : 0;
    let fmt = 0;
    if (l.fmt && l.fmt !== 'General') {
      const code = `<numFmt numFmtId="@" formatCode="${esc(l.fmt)}"/>`;
      let at = this.numFmts.indexOf(code);
      if (at < 0) at = this.numFmts.push(code) - 1;
      fmt = 164 + at;
    }
    const align = l.align || l.indent || l.wrap ? `<alignment${l.align ? ` horizontal="${l.align}"` : ''} vertical="center"${l.indent ? ` indent="${l.indent}"` : ''}${l.wrap ? ' wrapText="1"' : ''}/>` : '<alignment vertical="center"/>';
    this.xfs.push(`<xf numFmtId="${fmt}" fontId="${font}" fillId="${fill}" borderId="${border}" xfId="0" applyNumberFormat="1" applyFont="1" applyFill="1" applyBorder="1" applyAlignment="1">${align}</xf>`);
    this.index.set(key, this.xfs.length - 1);
    return this.xfs.length - 1;
  }
  xml(): string {
    return (
      XML +
      `<styleSheet xmlns="${NS}">` +
      (this.numFmts.length ? `<numFmts count="${this.numFmts.length}">${this.numFmts.map((x, i) => x.replace('"@"', `"${164 + i}"`)).join('')}</numFmts>` : '') +
      `<fonts count="${this.fonts.length}">${this.fonts.join('')}</fonts>` +
      `<fills count="${this.fills.length}">${this.fills.join('')}</fills>` +
      `<borders count="${this.borders.length}">${this.borders.join('')}</borders>` +
      '<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>' +
      `<cellXfs count="${this.xfs.length}">${this.xfs.join('')}</cellXfs>` +
      '<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>'
    );
  }
}

type Cell = { v: Value | Date; s: number } | undefined;

class Sheet {
  readonly rows: Cell[][] = [];
  readonly widths: number[] = [];
  readonly heights = new Map<number, number>();
  freeze?: { row: number; col: number };
  filter?: string;
  tab?: string;
  merges: string[] = [];
  links: { ref: string; location: string }[] = [];
  constructor(readonly name: string) {}
  set(r: number, c: number, v: Value | Date, s: number): void {
    (this.rows[r] ??= [])[c] = { v, s };
  }
  width(c: number, w: number): void {
    this.widths[c] = Math.max(this.widths[c] ?? 0, Math.min(w, 70));
  }
  xml(): string {
    const cells = (row: Cell[] | undefined, r: number): string => {
      if (!row) return '';
      let out = '';
      row.forEach((cell, c) => {
        if (!cell) return;
        const ref = col(c) + (r + 1);
        const v = cell.v;
        if (v === null || v === '') out += `<c r="${ref}" s="${cell.s}"/>`;
        else if (v instanceof Date) out += `<c r="${ref}" s="${cell.s}"><v>${v.getTime() / 86400000 + 25569}</v></c>`;
        else if (typeof v === 'number') out += Number.isFinite(v) ? `<c r="${ref}" s="${cell.s}"><v>${v}</v></c>` : `<c r="${ref}" s="${cell.s}"/>`;
        else if (typeof v === 'boolean') out += `<c r="${ref}" s="${cell.s}" t="b"><v>${v ? 1 : 0}</v></c>`;
        else out += `<c r="${ref}" s="${cell.s}" t="inlineStr"><is><t xml:space="preserve">${esc(v)}</t></is></c>`;
      });
      return out;
    };
    const last = this.rows.length;
    const lastCol = Math.max(0, ...this.rows.map(r => (r ? r.length : 0))) - 1;
    const pane = this.freeze
      ? `<pane${this.freeze.col ? ` xSplit="${this.freeze.col}"` : ''}${this.freeze.row ? ` ySplit="${this.freeze.row}"` : ''} topLeftCell="${col(this.freeze.col)}${this.freeze.row + 1}" activePane="${this.freeze.row && this.freeze.col ? 'bottomRight' : this.freeze.row ? 'bottomLeft' : 'topRight'}" state="frozen"/>`
      : '';
    return (
      XML +
      `<worksheet xmlns="${NS}" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">` +
      (this.tab ? `<sheetPr><tabColor rgb="FF${this.tab.slice(1).toUpperCase()}"/></sheetPr>` : '') +
      `<dimension ref="A1:${col(Math.max(lastCol, 0))}${Math.max(last, 1)}"/>` +
      `<sheetViews><sheetView showGridLines="0" workbookViewId="0">${pane}</sheetView></sheetViews>` +
      '<sheetFormatPr defaultRowHeight="15"/>' +
      (this.widths.length ? `<cols>${this.widths.map((w, i) => (w ? `<col min="${i + 1}" max="${i + 1}" width="${w.toFixed(1)}" customWidth="1"/>` : '')).join('')}</cols>` : '') +
      `<sheetData>${Array.from({ length: last }, (_, r) => {
        const h = this.heights.get(r);
        return `<row r="${r + 1}"${h ? ` ht="${h}" customHeight="1"` : ''}>${cells(this.rows[r], r)}</row>`;
      }).join('')}</sheetData>` +
      (this.filter ? `<autoFilter ref="${this.filter}"/>` : '') +
      (this.merges.length ? `<mergeCells count="${this.merges.length}">${this.merges.map(m => `<mergeCell ref="${m}"/>`).join('')}</mergeCells>` : '') +
      (this.links.length ? `<hyperlinks>${this.links.map(l => `<hyperlink ref="${l.ref}" location="${esc(l.location)}"/>`).join('')}</hyperlinks>` : '') +
      '<pageMargins left="0.5" right="0.5" top="0.6" bottom="0.6" header="0.3" footer="0.3"/>' +
      '<pageSetup orientation="landscape" fitToHeight="0"/>' +
      '</worksheet>'
    );
  }
}

const SLATE = '#37474f';
const BAND = '#f1f3f5';
const ZEBRA = '#fafafa';

/** A name Excel accepts for a sheet: at most 31 characters, none of []:*?/\, not taken. */
function sheetName(want: string, taken: Set<string>): string {
  const base = (want.replace(/[\[\]:*?/\\]/g, '_').replace(/^'+|'+$/g, '').trim() || 'Sheet').slice(0, 31);
  let name = base;
  for (let n = 2; taken.has(name.toLowerCase()); n++) name = base.slice(0, 31 - String(n).length - 1) + ' ' + n;
  taken.add(name.toLowerCase());
  return name;
}

/** The value for an Excel cell: a date pattern makes a date of text or numbers, anything else stays. */
function cellValue(v: Value | undefined, pattern: string | undefined): Value | Date {
  if (v === undefined) return null;
  if (pattern && /yyyy|yy|MM|dd|HH|mm|ss/.test(pattern) && v !== null && v !== '') return toDate(v) ?? v;
  return v;
}

export async function buildXlsx(info: WorkbookInfo): Promise<Uint8Array> {
  const styles = new Styles();
  const taken = new Set<string>();
  const first = new Sheet(sheetName('Report', taken));
  const sheets: Sheet[] = [first];
  const now = info.now ?? new Date();

  // ---- the first sheet ------------------------------------------------------------------------------------------
  let r = 0;
  const title = info.report.title || info.report.name;
  first.set(r++, 0, title, styles.style({ bold: true, size: 18, color: '#0d47a1' }));
  first.heights.set(0, 28);
  if (info.report.description) first.set(r++, 0, info.report.description, styles.style({ color: '#6b7280' }));
  first.set(r++, 0, `Generated ${formatValue(now.toISOString(), 'yyyy-MM-dd HH:mm')}`, styles.style({ color: '#6b7280', size: 9 }));
  const params = info.report.parameters.filter(p => info.params[p.name] !== null && info.params[p.name] !== undefined);
  if (params.length) {
    r++;
    first.set(r++, 0, 'Parameters', styles.style({ bold: true, color: SLATE }));
    for (const p of params) {
      first.set(r, 0, p.label || p.name, styles.style({ color: '#6b7280' }));
      first.set(r++, 1, info.params[p.name] as Value, styles.style({ bold: true, align: 'left' }));
    }
  }
  first.width(0, 30);
  first.width(1, 40);

  const body: { text: string; look: Look }[] = [];
  const own: { name: string; sheet: Sheet }[] = [];
  let tables = 0;

  // ---- a sheet per table, cross-tab and chart --------------------------------------------------------------------------
  const head = (sh: Sheet, titleText: string, note: string): number => {
    sh.set(0, 0, titleText, styles.style({ bold: true, size: 14, color: '#0d47a1' }));
    sh.heights.set(0, 24);
    sh.set(1, 0, note, styles.style({ color: '#6b7280', size: 9 }));
    return 3;
  };
  const headerLook = (align: Look['align']): Look => ({ bold: true, color: '#ffffff', fill: SLATE, align, wrap: true });

  walkBlocks(info.blocks, b => {
    if (b.t === 'text') {
      for (const p of b.paragraphs) {
        const text = plainText([p]);
        if (text.trim()) body.push({ text, look: { bold: b.look.face === 'bold' || p.runs.some(x => x.style.bold), size: Math.max(b.look.size, 10) > 14 ? 14 : 10, color: b.look.color === '#ffffff' ? '#222222' : b.look.color } });
      }
      return;
    }
    if (b.t === 'table') {
      const sh = new Sheet(sheetName(b.title, taken));
      sh.tab = '#4e79a7';
      const hr = head(sh, b.title, `${b.model.rows.filter(x => x.kind === 'data').length} rows · dataset ${b.item.dataset}`);
      const cols = b.model.columns;
      cols.forEach((c, i) => sh.set(hr, i, c.header, styles.style(headerLook(c.align))));
      sh.heights.set(hr, 22);
      sh.freeze = { row: hr + 1, col: 0 };
      cols.forEach((c, i) => sh.width(i, c.header.length * 1.2 + 4));
      let row = hr + 1;
      let zebra = 0;
      const plain = !b.item.groups?.length;
      for (const tr of b.model.rows as TableRow[]) {
        const group = tr.kind === 'group';
        const total = tr.kind === 'total' || tr.kind === 'grand';
        const fill = group ? BAND : total ? BAND : b.item.zebra && zebra++ % 2 ? ZEBRA : undefined;
        tr.cells.forEach((cell, i) => {
          const c = cols[i];
          const fmtPattern = c.def.format;
          const v: Value | Date = group && i === 0 ? cell.text : tr.kind === 'data' ? cellValue(tr.values![i], fmtPattern) : cell.text === '' ? null : cellValue(cell.value ?? cell.text, fmtPattern);
          const numeric = typeof v === 'number';
          const look: Look = {
            bold: group || total,
            fill,
            top: tr.kind === 'grand' ? 'medium' : tr.kind === 'total' ? 'thin' : undefined,
            fmt: numeric || v instanceof Date ? excelFormat(fmtPattern) : undefined,
            align: numeric || v instanceof Date ? (c.def.align ?? 'right') : cell.align,
            indent: i === 0 ? tr.level + (group ? 0 : tr.level ? 1 : 0) : undefined,
          };
          sh.set(row, i, v, styles.style(look));
          const shown = cell.text.length + 3;
          if (!group) sh.width(i, shown);
        });
        row++;
      }
      if (plain && b.model.rows.length) sh.filter = `A${hr + 1}:${col(cols.length - 1)}${row - (b.model.rows[b.model.rows.length - 1].kind === 'grand' ? 1 : 0)}`;
      sheets.push(sh);
      own.push({ name: sh.name, sheet: sh });
      tables++;
    } else if (b.t === 'matrix') {
      const sh = new Sheet(sheetName(b.title, taken));
      sh.tab = '#f28e2b';
      const m = b.model;
      const hr = head(sh, b.title, `${b.item.agg ?? 'sum'} of ${b.item.value} by ${b.item.rows.join(' / ')} and ${b.item.column}`);
      sh.set(hr, 0, m.corner, styles.style({ ...headerLook('left') }));
      m.columns.forEach((c, i) => sh.set(hr, i + 1, c.label, styles.style(headerLook('center'))));
      if (b.item.rowTotals) sh.set(hr, m.columns.length + 1, 'Total', styles.style(headerLook('center')));
      sh.heights.set(hr, 22);
      sh.freeze = { row: hr + 1, col: 1 };
      sh.width(0, 28);
      m.columns.forEach((c, i) => sh.width(i + 1, Math.max(12, c.label.length + 4)));
      const fmt = excelFormat(b.item.format ?? '#,##0.##');
      let row = hr + 1;
      for (const mr of m.rows) {
        const bold = mr.kind !== 'data';
        const fill = bold ? BAND : undefined;
        sh.set(row, 0, mr.label, styles.style({ bold, fill, indent: mr.level, top: mr.kind === 'total' ? 'thin' : undefined }));
        const values = [...mr.values, ...(b.item.rowTotals ? [mr.total] : [])];
        values.forEach((v, i) => {
          const isTotal = i >= m.columns.length;
          const heat = mr.kind === 'data' && !isTotal && v !== null ? heatFill(b.item.heat, v, m.min, m.max) : undefined;
          sh.set(row, i + 1, v, styles.style({ bold: bold || isTotal, fill: heat ?? (isTotal && mr.kind === 'data' ? BAND : fill), color: inkOn(heat), fmt, align: 'center', top: mr.kind === 'total' ? 'thin' : undefined }));
        });
        row++;
      }
      sheets.push(sh);
      own.push({ name: sh.name, sheet: sh });
      tables++;
    } else if (b.t === 'chart') {
      const sh = new Sheet(sheetName(`${b.title} (data)`, taken));
      sh.tab = '#59a14f';
      const hr = head(sh, b.title, 'The numbers behind the chart');
      sh.set(hr, 0, b.item.category, styles.style(headerLook('left')));
      b.data.series.forEach((s, i) => sh.set(hr, i + 1, s.label, styles.style(headerLook('right'))));
      sh.heights.set(hr, 22);
      sh.freeze = { row: hr + 1, col: 0 };
      sh.width(0, 24);
      b.data.series.forEach((s, i) => sh.width(i + 1, s.label.length + 6));
      const fmt = excelFormat(b.item.format ?? '#,##0.##');
      b.data.categories.forEach((cat, k) => {
        sh.set(hr + 1 + k, 0, cat, styles.style({}));
        b.data.series.forEach((s, i) => sh.set(hr + 1 + k, i + 1, s.values[k], styles.style({ fmt, align: 'right' })));
      });
      sh.filter = `A${hr + 1}:${col(b.data.series.length)}${hr + 1 + b.data.categories.length}`;
      sheets.push(sh);
      own.push({ name: sh.name, sheet: sh });
    }
  });

  if (body.length) {
    r++;
    for (const t of body) first.set(r++, 0, t.text, styles.style(t.look));
  }
  if (own.length) {
    r++;
    first.set(r++, 0, 'Sheets', styles.style({ bold: true, color: SLATE }));
    for (const o of own) {
      first.set(r, 0, o.name, styles.style({ color: '#1976d2', underline: true }));
      first.links.push({ ref: `A${r + 1}`, location: `'${o.name.replace(/'/g, "''")}'!A1` });
      r++;
    }
  }
  void tables;

  // ---- the package ----------------------------------------------------------------------------------------------------
  const rel = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships';
  const entries = [
    {
      name: '[Content_Types].xml',
      data: XML + '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/>' +
        '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>' +
        '<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>' +
        '<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>' +
        sheets.map((_, i) => `<Override PartName="/xl/worksheets/sheet${i + 1}.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>`).join('') + '</Types>',
    },
    {
      name: '_rels/.rels',
      data: XML + `<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="${rel}/officeDocument" Target="xl/workbook.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/></Relationships>`,
    },
    {
      name: 'docProps/core.xml',
      data: XML + `<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><dc:title>${esc(title)}</dc:title><dc:creator>libsql sql tool</dc:creator><dcterms:created xsi:type="dcterms:W3CDTF">${now.toISOString().replace(/\.\d+Z$/, 'Z')}</dcterms:created></cp:coreProperties>`,
    },
    {
      name: 'xl/workbook.xml',
      data: XML + `<workbook xmlns="${NS}" xmlns:r="${rel}"><bookViews><workbookView activeTab="0"/></bookViews><sheets>${sheets.map((s, i) => `<sheet name="${esc(s.name)}" sheetId="${i + 1}" r:id="rId${i + 1}"/>`).join('')}</sheets></workbook>`,
    },
    {
      name: 'xl/_rels/workbook.xml.rels',
      data: XML + `<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">${sheets.map((_, i) => `<Relationship Id="rId${i + 1}" Type="${rel}/worksheet" Target="worksheets/sheet${i + 1}.xml"/>`).join('')}<Relationship Id="rId${sheets.length + 1}" Type="${rel}/styles" Target="styles.xml"/></Relationships>`,
    },
    { name: 'xl/styles.xml', data: styles.xml() },
    ...sheets.map((s, i) => ({ name: `xl/worksheets/sheet${i + 1}.xml`, data: s.xml() })),
  ];
  return buildZip(entries, now);
}
