// Copyright (c) 2026 Philipp Orlov

import { newReport, ReportDef, validateReport, parseReport, isValidReportName } from './definition';
import { layoutReport, DataSet } from './layout';
import { testFonts } from './test-fonts';
import { TextPrim } from './primitives';

const fonts = testFonts();
const texts = (pages: { prims: any[] }[], i: number): string[] => pages[i].prims.filter(p => p.k === 'text').map((p: TextPrim) => p.text);

function sales(n: number): DataSet {
  const rows: (string | number)[][] = [];
  for (let i = 0; i < n; i++) rows.push([['East', 'West', 'North'][i % 3], `row ${i}`, i + 1]);
  return { columns: ['region', 'name', 'amount'], rows };
}

function report(body: ReportDef['body'], extra: Partial<ReportDef> = {}): ReportDef {
  return { ...newReport('Test'), datasets: [{ name: 'sales', sql: 'SELECT 1' }], body, ...extra };
}

const table = (extra: object = {}): ReportDef['body'][number] =>
  ({ type: 'table', dataset: 'sales', columns: [{ field: 'name', header: 'Name' }, { field: 'amount', header: 'Amount', total: 'sum', format: '#,##0' }], ...extra }) as ReportDef['body'][number];

describe('layoutReport', () => {
  it('numbers the pages in the footer and repeats the table heading on each', () => {
    const r = layoutReport({ report: report([table()]), params: {}, datasets: { sales: sales(200) }, fonts });
    expect(r.pages.length).toBeGreaterThan(2);
    r.pages.forEach((_, i) => {
      expect(texts(r.pages, i)).toContain(`Page ${i + 1} of ${r.pages.length}`);
      expect(texts(r.pages, i)).toContain('Amount');
      expect(texts(r.pages, i)).toContain('Test'); // the page header
    });
    const rowsShown = r.pages.reduce((n, _, i) => n + texts(r.pages, i).filter(t => /^row \d+$/.test(t)).length, 0);
    expect(rowsShown).toBe(200);
    expect(r.warnings).toEqual([]);
  });

  it('keeps everything inside the margins', () => {
    const r = layoutReport({ report: report([table()]), params: {}, datasets: { sales: sales(120) }, fonts });
    const rep = report([]);
    const m = rep.page.margins;
    for (const page of r.pages)
      for (const p of page.prims) {
        if (p.k === 'text') {
          expect(p.y).toBeLessThanOrEqual(page.height - m.bottom + 1);
          expect(p.x).toBeGreaterThanOrEqual(m.left - 0.01);
        }
      }
  });

  it('groups with headings and subtotals, sorted, plus a grand total', () => {
    const r = layoutReport({
      report: report([table({ groups: [{ field: 'region', header: '{{region}}: {{Count()}} rows', subtotals: true }], grandTotal: true })]),
      params: {},
      datasets: { sales: sales(9) },
      fonts,
    });
    const t = texts(r.pages, 0);
    expect(t).toContain('East: 3 rows');
    expect(t.indexOf('East: 3 rows')).toBeLessThan(t.indexOf('North: 3 rows'));
    expect(t.indexOf('North: 3 rows')).toBeLessThan(t.indexOf('West: 3 rows'));
    expect(t).toContain('Total East');
    expect(t).toContain('Total'); // grand total
    expect(t).toContain('45'); // 1+..+9
    // East = rows 0,3,6 -> amounts 1,4,7
    expect(t).toContain('12');
  });

  it('computes columns from expressions and formats cells', () => {
    const r = layoutReport({
      report: report([{ type: 'table', dataset: 'sales', columns: [{ field: 'name' }, { expression: 'amount * 1000', header: 'Big', format: '#,##0' }] }]),
      params: {},
      datasets: { sales: sales(2) },
      fonts,
    });
    expect(texts(r.pages, 0)).toContain('2,000');
  });

  it('wraps long text and breaks long words', () => {
    const long = 'word '.repeat(200);
    const r = layoutReport({ report: report([{ type: 'text', text: long }, { type: 'text', text: 'x'.repeat(400) }]), params: {}, datasets: {}, fonts });
    const lines = texts(r.pages, 0).filter(t => t.startsWith('word'));
    expect(lines.length).toBeGreaterThan(5);
    expect(lines.join(' ').replace(/ +/g, ' ').trim().split(' ').length).toBe(200);
    expect(texts(r.pages, 0).filter(t => /^x+$/.test(t)).length).toBeGreaterThan(1);
  });

  it('uses parameters and dataset aggregates in text', () => {
    const r = layoutReport({
      report: report([{ type: 'text', text: 'Year {{year}}: {{Sum(amount)}} over {{Count()}} rows', dataset: 'sales' }]),
      params: { year: 2026 },
      datasets: { sales: sales(4) },
      fonts,
    });
    expect(texts(r.pages, 0)).toContain('Year 2026: 10 over 4 rows');
  });

  it('draws charts and says what is missing', () => {
    const r = layoutReport({
      report: report([
        { type: 'chart', chart: 'bar', dataset: 'sales', category: 'name', series: [{ field: 'amount' }], height: 150, title: 'Bars' },
        { type: 'chart', chart: 'pie', dataset: 'sales', category: 'region', series: [{ field: 'amount' }], height: 150 },
        { type: 'chart', chart: 'line', dataset: 'gone', category: 'x', series: [{ field: 'y' }], height: 100 },
        { type: 'table', dataset: 'sales', columns: [{ field: 'nope' }] },
      ]),
      params: {},
      datasets: { sales: sales(6) },
      fonts,
    });
    expect(texts(r.pages, 0)).toContain('Bars');
    expect(r.pages[0].prims.filter(p => p.k === 'path').length).toBeGreaterThanOrEqual(3); // pie slices
    expect(r.warnings.some(w => /dataset gone/.test(w))).toBe(true);
    expect(r.warnings.some(w => /no column nope/.test(w))).toBe(true);
  });

  it('honors page breaks and page-break groups', () => {
    const r = layoutReport({ report: report([{ type: 'text', text: 'one' }, { type: 'pageBreak' }, { type: 'text', text: 'two' }]), params: {}, datasets: {}, fonts });
    expect(r.pages.length).toBe(2);
    const g = layoutReport({ report: report([table({ groups: [{ field: 'region', pageBreak: true }] })]), params: {}, datasets: { sales: sales(9) }, fonts });
    expect(g.pages.length).toBe(3);
  });

  it('stops at the page limit', () => {
    const r = layoutReport({ report: report([table()]), params: {}, datasets: { sales: sales(5000) }, fonts, maxPages: 3 });
    expect(r.pages.length).toBe(3);
    expect(r.warnings.join(' ')).toMatch(/cut at 3 pages/);
  });

  it('shows an empty table and handles landscape', () => {
    const rep = report([table()]);
    rep.page.orientation = 'landscape';
    const r = layoutReport({ report: rep, params: {}, datasets: { sales: { columns: ['name', 'amount'], rows: [] } }, fonts });
    expect(texts(r.pages, 0)).toContain('No rows');
    expect(r.pages[0].width).toBeGreaterThan(r.pages[0].height);
  });
});

describe('totals', () => {
  it('labels the total in the first text column, not in a narrow number column before it', () => {
    const rep = report([{ type: 'table', dataset: 'sales', grandTotal: true, columns: [{ field: 'amount', header: '#', width: 0.3, align: 'center' }, { field: 'name', header: 'Name' }, { field: 'amount', header: 'Amount', total: 'sum' }] }]);
    const r = layoutReport({ report: rep, params: {}, datasets: { sales: sales(3) }, fonts });
    const prims = r.pages[0].prims.filter((p): p is TextPrim => p.k === 'text');
    const total = prims.find(p => p.text === 'Total')!;
    const name = prims.find(p => p.text === 'Name')!; // the heading of the second column
    expect(total.x).toBeCloseTo(name.x, 1); // under the Name column, not under '#'
  });
});

describe('definitions', () => {
  it('accepts a new report and the one used above', () => {
    expect(validateReport(newReport('Fine'))).toEqual([]);
  });
  it('lists what is wrong', () => {
    const r = report([{ type: 'table', dataset: 'missing', columns: [] }, { type: 'chart', chart: 'bar', dataset: 'sales', category: '', series: [], height: 10 }, { type: 'text', text: '{{ 1 + }}' }], {
      name: '../x',
      parameters: [{ name: 'a b', type: 'text' }],
      datasets: [{ name: 'sales', sql: 'SELECT :undeclared' }],
    });
    const errors = validateReport(r).join('\n');
    expect(errors).toMatch(/name must be/);
    expect(errors).toMatch(/not an identifier/);
    expect(errors).toMatch(/uses :undeclared/);
    expect(errors).toMatch(/dataset 'missing'/);
    expect(errors).toMatch(/no columns/);
    expect(errors).toMatch(/no category/);
    expect(errors).toMatch(/no series/);
    expect(errors).toMatch(/height must be/);
    expect(errors).toMatch(/unexpected/);
  });
  it('restricts report names to safe file names', () => {
    for (const ok of ['Sales 2026', 'a', 'q3_report-final.v2']) expect(isValidReportName(ok)).toBe(true);
    for (const bad of ['', '.hidden', '../etc', 'a/b', 'a\\b', 'x'.repeat(81), 'name.', 'a..b', 'tab\t']) expect(isValidReportName(bad)).toBe(false);
  });
  it('parses stored JSON, filling in what an older file lacks', () => {
    const r = parseReport('{"format":1,"name":"Old","body":[]}');
    expect(r.page.size).toBe('A4');
    expect(r.parameters).toEqual([]);
    expect(() => parseReport('nope')).toThrowError(/not valid JSON/);
    expect(() => parseReport('3')).toThrowError(/JSON object/);
  });
});

import { tableData, tableToCsv } from './export';

describe('tableData', () => {
  it('gives each table the rows in the order the report shows them, computed columns included', () => {
    const rep = report([
      { type: 'text', text: 'x' },
      { type: 'table', dataset: 'sales', groups: [{ field: 'region', sort: 'desc' }], columns: [{ field: 'region', header: 'Region' }, { expression: 'amount * 2', header: 'Double' }] },
    ]);
    const [t] = tableData(rep, {}, { sales: sales(6) });
    expect(t.title).toBe('Table 2 (sales)');
    expect(t.columns).toEqual(['Region', 'Double']);
    expect(t.rows.map(r => r[0])).toEqual(['West', 'West', 'North', 'North', 'East', 'East']);
    expect(t.rows.map(r => r[1])).toEqual([4, 10, 6, 12, 2, 8]); // amounts 2,5 | 3,6 | 1,4 doubled; stable inside a group
    expect(tableToCsv(t).startsWith('﻿Region,Double\r\n')).toBe(true);
  });
  it('skips a table whose dataset is missing', () => {
    expect(tableData(report([table()]), {}, {})).toEqual([]);
  });
});
