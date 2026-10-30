// Copyright (c) 2026 Philipp Orlov

import * as fs from 'node:fs';
import * as path from 'node:path';
import * as zlib from 'node:zlib';
import { buildBlocks, walkBlocks } from './blocks';
import { BodyItem, newReport, ReportDef } from './definition';
import { buildDocx } from './docx';
import { tableData, tableToCsv } from './export';
import { decodePng } from './image';
import { DataSet, layoutReport } from './layout';
import { buildPdf } from './pdf';
import { testFonts } from './test-fonts';
import { buildXlsx, excelFormat } from './xlsx';
import { readZip } from './zip';

const fonts = testFonts();
const decoder = new TextDecoder();

const sales: DataSet = {
  columns: ['region', 'quarter', 'product', 'amount', 'day'],
  rows: [
    ['East', 'Q1', 'A', 10, '2026-01-05'], ['East', 'Q2', 'A', 20, '2026-04-06'], ['East', 'Q1', 'B', 5.5, '2026-02-07'],
    ['West', 'Q1', 'A', 40, '2026-01-09'], ['West', 'Q2', 'B', 30, '2026-05-10'], ['West', 'Q3', 'B', 1, '2026-08-11'],
  ],
};

/** A PNG of 2x2 pixels with an alpha channel, made with zlib. */
function png(): string {
  const crcTable = Array.from({ length: 256 }, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
  });
  const crc = (b: Buffer): number => {
    let c = 0xffffffff;
    for (const x of b) c = crcTable[(c ^ x) & 255] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };
  const chunk = (type: string, data: Buffer): Buffer => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type), data]);
    const sum = Buffer.alloc(4);
    sum.writeUInt32BE(crc(body));
    return Buffer.concat([len, body, sum]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(2, 0);
  ihdr.writeUInt32BE(2, 4);
  ihdr[8] = 8;
  ihdr[9] = 6;
  // two rows, filter 0 (none) and filter 1 (sub)
  const raw = Buffer.from([0, 255, 0, 0, 255, 0, 255, 0, 128, 1, 0, 0, 255, 255, 0, 0, 0, 0]);
  const file = Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
  return 'data:image/png;base64,' + file.toString('base64');
}
const svg = 'data:image/svg+xml;base64,' + Buffer.from('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 40 20"><rect width="40" height="20" fill="#e15759"/></svg>').toString('base64');

function demo(): { report: ReportDef; datasets: Record<string, DataSet> } {
  const body: BodyItem[] = [
    { type: 'text', rich: true, text: '[size=20][b]Hello[/b][/size] [color=#e15759]world[/color]\n- one\n- [u]two[/u]', style: { background: '#e3f2fd' } },
    { type: 'image', src: png(), width: 40 },
    { type: 'image', src: svg, width: 80, caption: 'A logo' },
    { type: 'columns', columns: [
      { card: true, accent: '#4e79a7', items: [{ type: 'text', text: 'Left card' }, { type: 'chart', chart: 'bar', dataset: 'sales', category: 'region', series: [{ field: 'amount' }], height: 120, title: 'Amounts' }] },
      { items: [{ type: 'matrix', dataset: 'sales', rows: ['region'], column: 'quarter', value: 'amount', rowTotals: true, columnTotals: true, title: 'Matrix' }] },
    ] },
    { type: 'table', dataset: 'sales', zebra: true, grandTotal: true, groups: [{ field: 'region', subtotals: true }],
      columns: [{ field: 'product' }, { field: 'day', format: 'yyyy-MM-dd' }, { field: 'amount', total: 'sum', format: '#,##0.00' }] },
  ];
  return { report: { ...newReport('Demo'), datasets: [{ name: 'sales', sql: 'x' }], body }, datasets: { sales } };
}

const out = process.env['EXPORT_OUT'];
const save = (name: string, bytes: Uint8Array): void => {
  if (out) fs.writeFileSync(path.join(out, name), bytes);
};

describe('table data and CSV', () => {
  it('lists every table, nested ones too, with the displayed order and unformatted values', () => {
    const { report, datasets } = demo();
    const t = tableData(report, {}, datasets);
    expect(t.length).toBe(1);
    expect(t[0].columns).toEqual(['product', 'day', 'amount']);
    expect(t[0].rows.map(r => r[2])).toEqual([10, 20, 5.5, 40, 30, 1]);
    expect(tableToCsv(t[0]).split('\r\n')[1]).toBe('﻿A,2026-01-05,10'.slice(1));
  });
});

describe('PNG and PDF pictures', () => {
  it('decodes a PNG with alpha and unfilters it', async () => {
    const d = Buffer.from(png().split(',')[1], 'base64');
    const r = await decodePng(new Uint8Array(d));
    expect([r.width, r.height]).toEqual([2, 2]);
    expect([...r.rgb]).toEqual([255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 255]); // red, green; blue twice (the second row is "sub"-filtered)
    expect([...r.alpha!]).toEqual([255, 128, 255, 255]);
  });

  it('embeds pictures in the PDF: PNG as pixels with a soft mask, SVG through the rasterizer, and leaves out what it cannot read', async () => {
    const { report, datasets } = demo();
    const laid = layoutReport({ report, params: {}, datasets, fonts });
    const asked: string[] = [];
    const pdf = await buildPdf(laid.pages, fonts, {}, { rasterize: async (src, w, h) => (asked.push(src.slice(0, 20)), { width: 2, height: 1, rgb: new Uint8Array(6) }) });
    save('demo.pdf', pdf);
    const text = Buffer.from(pdf).toString('latin1');
    expect((text.match(/\/Subtype \/Image/g) ?? []).length).toBe(3); // the PNG, its mask, the SVG
    expect(text).toContain('/SMask');
    expect(text).toContain('/XObject');
    expect(asked).toEqual(['data:image/svg+xml;b']);
    const without = await buildPdf(laid.pages, fonts);
    expect((Buffer.from(without).toString('latin1').match(/\/Subtype \/Image/g) ?? []).length).toBe(2); // the SVG is left out, the PNG and its mask stay
  });
});

describe('Excel', () => {
  it('turns report patterns into Excel formats', () => {
    expect(excelFormat('#,##0.00')).toBe('#,##0.00');
    expect(excelFormat('$#,##0')).toBe('$#,##0');
    expect(excelFormat('0.0%')).toBe('0.0%');
    expect(excelFormat('yyyy-MM-dd HH:mm')).toBe('yyyy-mm-dd hh:mm');
    expect(excelFormat('#,##0 units')).toBe('#,##0 "units"');
    expect(excelFormat(undefined)).toBe('General');
  });

  it('writes a workbook with a sheet per table, cross-tab and chart', async () => {
    const { report, datasets } = demo();
    const { blocks } = buildBlocks({ report, params: {}, datasets });
    const bytes = await buildXlsx({ report, params: {}, blocks, now: new Date('2026-10-30T10:00:00Z') });
    save('demo.xlsx', bytes);
    const files = await readZip(bytes);
    expect([...files.keys()]).toEqual(jasmine.arrayContaining(['[Content_Types].xml', 'xl/workbook.xml', 'xl/styles.xml', 'xl/worksheets/sheet1.xml', 'xl/worksheets/sheet4.xml']));
    const wb = decoder.decode(files.get('xl/workbook.xml'));
    expect(wb).toContain('name="Report"');
    expect(wb).toContain('name="Matrix"');
    expect(wb).toContain('Amounts (data)');
    expect(wb).toMatch(/Table 5 \(sales\)/);
    const sheetOf = (n: string): string => decoder.decode(files.get(`xl/worksheets/sheet${n}.xml`));
    const tableSheet = [...files.keys()].filter(k => k.startsWith('xl/worksheets/')).map(k => decoder.decode(files.get(k))).find(x => x.includes('>day<'))!;
    expect(tableSheet).toContain('<pane ySplit="4"');
    expect(tableSheet).toContain('<v>10</v>');
    expect(tableSheet).toContain('<v>46027</v>'); // 2026-01-05 as an Excel date
    expect(sheetOf('1')).toContain('Hello world');
    const styles = decoder.decode(files.get('xl/styles.xml'));
    expect(styles).toContain('formatCode="yyyy-mm-dd"');
    expect(styles).toContain('formatCode="#,##0.00"');
  });
});

describe('Word', () => {
  it('writes a document with paragraphs, tables, a picture and the columns', async () => {
    const { report, datasets } = demo();
    const { blocks } = buildBlocks({ report, params: {}, datasets });
    const pngBytes = new Uint8Array(Buffer.from(png().split(',')[1], 'base64'));
    const bytes = await buildDocx({ report, params: {}, blocks }, { rasterize: async () => pngBytes, chart: async () => ({ png: pngBytes, width: 200, height: 100 }), now: new Date('2026-10-30T10:00:00Z') });
    save('demo.docx', bytes);
    const files = await readZip(bytes);
    expect([...files.keys()]).toEqual(jasmine.arrayContaining(['word/document.xml', 'word/styles.xml', 'word/header1.xml', 'word/footer1.xml']));
    expect([...files.keys()].filter(k => k.startsWith('word/media/')).length).toBe(3); // the PNG, the rasterized SVG, the chart
    const doc = decoder.decode(files.get('word/document.xml'));
    expect(doc).toContain('Hello');
    expect(doc).toContain('<w:color w:val="E15759"/>');
    expect((doc.match(/<w:tbl>/g) ?? []).length).toBeGreaterThanOrEqual(4); // columns, the card, the matrix, the table
    expect(doc).toContain('w:tblHeader');
    expect(doc).toContain('<w:pgSz w:w="11906"');
    expect(doc).toContain('Total West');
    const types = decoder.decode(files.get('[Content_Types].xml'));
    expect(types).toContain('Extension="png"');
  });

  it('falls back to a data table for a chart that cannot be drawn', async () => {
    const { report, datasets } = demo();
    const { blocks } = buildBlocks({ report, params: {}, datasets });
    let charts = 0;
    walkBlocks(blocks, b => b.t === 'chart' && charts++);
    expect(charts).toBe(1);
    const files = await readZip(await buildDocx({ report, params: {}, blocks }));
    expect(decoder.decode(files.get('word/document.xml'))).toContain('>East<');
  });
});
