// Copyright (c) 2026 Philipp Orlov

// A report as an Excel workbook: a first sheet with the title, the parameters and the text, and a sheet of its own for
// every table, matrix and chart. The workbook writer (styles, sheets, packaging) is @ui-kit/core/export's; this file
// only walks the report's blocks. Free of Angular imports on purpose.

import { XlsxLook, XlsxSheet, XlsxStyles, buildWorkbook, columnName, excelFormat, sheetName, xlsxCellValue } from '@ui-kit/core/export';
import { formatValue, Value } from '@ui-kit/core/format';

import { Block, walkBlocks } from './blocks';
import { ReportDef } from './definition';
import { heatFill, inkOn } from './matrix';
import { plainText } from './richtext';
import { TableRow } from './tablemodel';

export { excelFormat };

export interface WorkbookInfo {
  report: ReportDef;
  params: Record<string, Value>;
  blocks: Block[];
  now?: Date;
}

const SLATE = '#37474f';
const BAND = '#f1f3f5';
const ZEBRA = '#fafafa';



export async function buildXlsx(info: WorkbookInfo): Promise<Uint8Array> {
  const styles = new XlsxStyles();
  const taken = new Set<string>();
  const first = new XlsxSheet(sheetName('Report', taken));
  const sheets: XlsxSheet[] = [first];
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

  const body: { text: string; look: XlsxLook }[] = [];
  const own: { name: string; sheet: XlsxSheet }[] = [];
  let tables = 0;

  // ---- a sheet per table, cross-tab and chart --------------------------------------------------------------------------
  const head = (sh: XlsxSheet, titleText: string, note: string): number => {
    sh.set(0, 0, titleText, styles.style({ bold: true, size: 14, color: '#0d47a1' }));
    sh.heights.set(0, 24);
    sh.set(1, 0, note, styles.style({ color: '#6b7280', size: 9 }));
    return 3;
  };
  const headerLook = (align: XlsxLook['align']): XlsxLook => ({ bold: true, color: '#ffffff', fill: SLATE, align, wrap: true });

  walkBlocks(info.blocks, b => {
    if (b.t === 'text') {
      for (const p of b.paragraphs) {
        const text = plainText([p]);
        if (text.trim()) body.push({ text, look: { bold: b.look.face === 'bold' || p.runs.some(x => x.style.bold), size: Math.max(b.look.size, 10) > 14 ? 14 : 10, color: b.look.color === '#ffffff' ? '#222222' : b.look.color } });
      }
      return;
    }
    if (b.t === 'table') {
      const sh = new XlsxSheet(sheetName(b.title, taken));
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
          const v: Value | Date = group && i === 0 ? cell.text : tr.kind === 'data' ? xlsxCellValue(tr.values![i], fmtPattern) : cell.text === '' ? null : xlsxCellValue(cell.value ?? cell.text, fmtPattern);
          const numeric = typeof v === 'number';
          const look: XlsxLook = {
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
      if (plain && b.model.rows.length) sh.filter = `A${hr + 1}:${columnName(cols.length - 1)}${row - (b.model.rows[b.model.rows.length - 1].kind === 'grand' ? 1 : 0)}`;
      sheets.push(sh);
      own.push({ name: sh.name, sheet: sh });
      tables++;
    } else if (b.t === 'matrix') {
      const sh = new XlsxSheet(sheetName(b.title, taken));
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
      const sh = new XlsxSheet(sheetName(`${b.title} (data)`, taken));
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
      sh.filter = `A${hr + 1}:${columnName(b.data.series.length)}${hr + 1 + b.data.categories.length}`;
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

  return buildWorkbook(sheets, styles, { title, creator: 'libsql sql tool', now });
}
