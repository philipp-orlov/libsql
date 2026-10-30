// Copyright (c) 2026 Philipp Orlov

import { Measurer } from './chart';
import { BodyItem, newReport, ReportDef, validateReport } from './definition';
import { decodeDataUri, imageInfo } from './image';
import { DataSet, layoutReport } from './layout';
import { heatFill, inkOn, matrixModel } from './matrix';
import { Action, Prim, TextPrim } from './primitives';
import { checkRich, parseRich, plainText, wrapRich } from './richtext';
import { resolveSubreports, subKey } from './subreport';
import { testFonts } from './test-fonts';

const fonts = testFonts();
const m: Measurer = { width: (t, f, s) => fonts[f].measure(t, s) };

const svg = (w: number, h: number): string => 'data:image/svg+xml;base64,' + Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}"><rect width="${w}" height="${h}" fill="#4e79a7"/></svg>`).toString('base64');

const sales: DataSet = {
  columns: ['region', 'quarter', 'product', 'amount'],
  rows: [
    ['East', 'Q1', 'A', 10], ['East', 'Q2', 'A', 20], ['East', 'Q1', 'B', 5],
    ['West', 'Q1', 'A', 40], ['West', 'Q2', 'B', 30], ['West', 'Q3', 'B', 1],
  ],
};

function report(body: BodyItem[], extra: Partial<ReportDef> = {}): ReportDef {
  return { ...newReport('Test'), datasets: [{ name: 'sales', sql: 'SELECT 1' }], header: [], footer: [], body, ...extra };
}
const lay = (body: BodyItem[], extra: Partial<ReportDef> = {}, more = {}) => layoutReport({ report: report(body, extra), params: {}, datasets: { sales }, fonts, ...more });
const texts = (prims: Prim[]): TextPrim[] => prims.filter((p): p is TextPrim => p.k === 'text');
const actions = (prims: Prim[]): Action[] => prims.flatMap(p => ('hit' in p && p.hit?.action ? [p.hit.action] : []));

describe('rich text', () => {
  const fill = (e: string): string => `<${e}>`;
  it('reads bold, underline, color, size and highlight, nested', () => {
    const p = parseRich('a [b]B [color=#ff0000]R[/color][/b] [size=20]big[/] [bg=#ffff00][u]hu[/u][/bg]', fill);
    expect(p.length).toBe(1);
    const runs = p[0].runs;
    expect(runs.map(r => r.text)).toEqual(['a ', 'B ', 'R', ' ', 'big', ' ', 'hu']);
    expect(runs[1].style).toEqual({ bold: true });
    expect(runs[2].style).toEqual({ bold: true, color: '#ff0000' });
    expect(runs[4].style).toEqual({ size: 20 });
    expect(runs[6].style).toEqual({ bg: '#ffff00', underline: true });
  });

  it('does not read markup out of the values of expressions, and takes backslashes literally', () => {
    const p = parseRich('x {{v}} \\[b] y', e => '[b]' + e);
    expect(plainText(p)).toBe('x [b]v [b] y');
    expect(p[0].runs.every(r => !r.style.bold)).toBe(true);
  });

  it('makes bullets of lines that start with "- "', () => {
    const p = parseRich('Title\n- one\n- [b]two[/b]', fill);
    expect(p.map(x => x.bullet)).toEqual([false, true, true]);
    expect(plainText(p)).toBe('Title\n• one\n• two');
  });

  it('checks the markup', () => {
    expect(checkRich('[b]x[/b] [color=#abc]y[/color]')).toBeNull();
    expect(checkRich('[b]never closed')).toBeNull();
    expect(checkRich('x[/b]')).toMatch(/closes nothing/);
    expect(checkRich('[b]x[/u]')).toMatch(/where/);
    expect(checkRich('[color=red]x[/color]')).toMatch(/color/);
    expect(checkRich('[size=2]x[/size]')).toMatch(/size/);
  });

  it('wraps mixed styles into lines and sizes a line by its biggest run', () => {
    const lines = wrapRich(parseRich('small words here [size=30]BIG[/size] and more small words to wrap around', fill), 160, { face: 'regular', size: 10, color: '#000' }, m);
    expect(lines.length).toBeGreaterThan(1);
    expect(lines.every(l => l.width <= 160.01)).toBe(true);
    expect(lines.find(l => l.fragments.some(f => f.text.includes('BIG')))!.size).toBe(30);
  });

  it('lays a rich text item out with bold fragments in the bold face', () => {
    const r = lay([{ type: 'text', rich: true, text: 'plain [b]strong[/b] [color=#e15759]red[/color]' }]);
    const t = texts(r.pages[0].prims);
    expect(t.find(x => x.text.includes('strong'))!.face).toBe('bold');
    expect(t.find(x => x.text.includes('red'))!.color).toBe('#e15759');
    expect(t.find(x => x.text.includes('plain'))!.face).toBe('regular');
  });
});

describe('images', () => {
  it('reads sizes from the headers', () => {
    const info = imageInfo(svg(120, 60))!;
    expect([info.kind, info.width, info.height]).toEqual(['svg', 120, 60]);
    const png = Buffer.alloc(33);
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]).copy(png);
    png.writeUInt32BE(13, 8);
    png.write('IHDR', 12);
    png.writeUInt32BE(30, 16);
    png.writeUInt32BE(20, 20);
    const p = imageInfo('data:image/png;base64,' + png.toString('base64'))!;
    expect([p.kind, p.width, p.height]).toEqual(['png', 30, 20]);
    expect(imageInfo('data:image/png;base64,AAAA')).toBeNull();
    expect(decodeDataUri('http://x')).toBeNull();
  });

  it('places a picture at its own size, or at the given width keeping the proportions, and shrinks it to the page', () => {
    const img = (extra: object) => lay([{ type: 'image', src: svg(200, 100), ...extra } as BodyItem]).pages[0].prims.find(p => p.k === 'image') as Prim & { w: number; h: number; x: number };
    expect([img({}).w, img({}).h]).toEqual([150, 75]); // 96 dpi pixels in points
    expect([img({ width: 100 }).w, img({ width: 100 }).h]).toEqual([100, 50]);
    expect(img({ width: 5000 / 10, align: 'right' }).x).toBeGreaterThan(40);
    const wide = img({ width: 1900 });
    expect(wide.w).toBeCloseTo(595.28 - 80, 1); // the page's content width
  });

  it('reads a picture out of a text field, and says nothing when it is empty', () => {
    const ds: DataSet = { columns: ['icon'], rows: [[svg(10, 10)], [null]] };
    const r = layoutReport({ report: report([{ type: 'image', src: '{{First(icon)}}', dataset: 'icons' }, { type: 'image', src: '{{Last(icon)}}', dataset: 'icons' }]), params: {}, datasets: { icons: ds }, fonts });
    expect(r.pages[0].prims.filter(p => p.k === 'image').length).toBe(1);
    expect(r.warnings).toEqual([]);
  });

  it('warns about a picture it cannot read', () => {
    const r = lay([{ type: 'image', src: 'data:image/png;base64,AAAA' }]);
    expect(r.warnings.join()).toMatch(/image/);
  });
});

describe('matrix', () => {
  const item = { type: 'matrix', dataset: 'sales', rows: ['region'], column: 'quarter', value: 'amount', rowTotals: true, columnTotals: true } as const;

  it('pivots, totals and keeps the order of the data', () => {
    const model = matrixModel({ ...item }, sales, 'matrix 1', 20);
    expect(model.columns.map(c => c.label)).toEqual(['Q1', 'Q2', 'Q3']);
    const east = model.rows.find(r => r.label === 'East')!;
    expect(east.values).toEqual([15, 20, null]);
    expect(east.total).toBe(35);
    const total = model.rows[model.rows.length - 1];
    expect([total.label, total.values, total.total]).toEqual(['Total', [55, 50, 1], 106]);
    expect([model.min, model.max]).toEqual([1, 40]);
  });

  it('aggregates by average, sorts rows by total and columns descending', () => {
    const model = matrixModel({ ...item, agg: 'avg', sortRows: 'total', sortColumns: 'desc' }, sales, 'm', 20);
    expect(model.columns.map(c => c.label)).toEqual(['Q3', 'Q2', 'Q1']);
    expect(model.rows[0].label).toBe('West');
    expect(model.rows[0].values).toEqual([1, 30, 40]);
    expect(model.rows[0].total).toBeCloseTo(71 / 3);
  });

  it('nests a second row field under the first, with subtotals on the group line', () => {
    const model = matrixModel({ ...item, rows: ['region', 'product'], subtotals: true }, sales, 'm', 20);
    expect(model.rows.map(r => `${r.kind}:${r.label}`)).toEqual(['group:East', 'data:A', 'data:B', 'group:West', 'data:A', 'data:B', 'total:Total']);
    expect(model.rows[0].values).toEqual([15, 20, null]);
    expect(model.rows[1].keys).toEqual(['East', 'A']);
  });

  it('cuts the columns that do not fit and says so', () => {
    const model = matrixModel(item, sales, 'matrix 2', 2);
    expect(model.columns.length).toBe(2);
    expect(model.cut).toBe(1);
    expect(model.warnings.join()).toMatch(/first 2 of 3/);
  });

  it('colors from light to dark and picks readable ink', () => {
    expect(heatFill('blue', 0, 0, 10)).not.toBe(heatFill('blue', 10, 0, 10));
    expect(inkOn(heatFill('blue', 10, 0, 10))).toBe('#ffffff');
    expect(inkOn(heatFill('blue', 0, 0, 10))).toBe('#222222');
    expect(heatFill('none', 1, 0, 2)).toBeUndefined();
    expect(heatFill('diverging', -5, -5, 5)).not.toBe(heatFill('diverging', 5, -5, 5));
  });

  it('is drawn: headings, cells with their numbers, a legend, and clicks that sort', () => {
    const r = lay([{ ...item, title: 'By region', format: '#,##0' }]);
    const t = texts(r.pages[0].prims).map(x => x.text);
    expect(t).toEqual(jasmine.arrayContaining(['By region', 'Q1', 'Q2', 'Q3', 'Total', 'East', 'West', '40', '106']));
    expect(actions(r.pages[0].prims).some(a => a.type === 'sort')).toBe(true);
    const cells = r.pages[0].prims.filter(p => p.k === 'rect' && p.hit?.cls === 'cell');
    expect(cells.length).toBe(5); // the body cells that hold a number
    const sorted = lay([{ ...item }], {}, { view: { sort: { '0': { column: 0, dir: 'desc' } } } });
    const rows = texts(sorted.pages[0].prims).map(x => x.text).filter(x => x === 'East' || x === 'West');
    expect(rows).toEqual(['West', 'East']);
  });

  it('drills through with the row, column and value of the cell', () => {
    const r = lay([{ ...item, drill: { report: 'Detail', params: { region: '{{region}}', q: '{{quarter}}', amount: '{{amount}}' } } }]);
    const drills = actions(r.pages[0].prims).filter((a): a is Extract<Action, { type: 'drill' }> => a.type === 'drill');
    expect(drills.length).toBe(10); // the number and the tinted cell behind it
    expect(drills[0]).toEqual({ type: 'drill', report: 'Detail', params: { region: 'East', q: 'Q1', amount: '15' } });
  });
});

describe('drill-through', () => {
  it('puts the row of the dataset behind a chart mark or a table row into the target\'s parameters', () => {
    const drill = { report: 'Region', params: { region: '{{region}}', n: '{{amount}}' } };
    const r = lay([
      { type: 'chart', chart: 'bar', dataset: 'sales', category: 'region', series: [{ field: 'amount' }], height: 200, drill },
      { type: 'table', dataset: 'sales', columns: [{ field: 'region' }, { field: 'amount' }], drill },
    ]);
    const a = actions(r.pages[0].prims).filter(x => x.type === 'drill') as Extract<Action, { type: 'drill' }>[];
    expect(a.length).toBeGreaterThan(12); // bars, slots, rows
    expect(a[0].params).toEqual({ region: 'East', n: '10' });
    expect(a[a.length - 1].params).toEqual({ region: 'West', n: '1' });
  });

  it('validates the target and its parameters', () => {
    const bad = report([{ type: 'table', dataset: 'sales', columns: [{ field: 'region' }], drill: { report: '', params: { a: '{{(}}' } } }]);
    expect(validateReport(bad).join()).toMatch(/no target report/);
  });
});

describe('columns', () => {
  const text = (t: string): BodyItem => ({ type: 'text', text: t });

  it('stands items side by side, each in its own share of the width', () => {
    const r = lay([{ type: 'columns', gap: 10, columns: [{ width: 1, items: [text('left')] }, { width: 3, items: [text('right')] }] }]);
    const [l, rt] = ['left', 'right'].map(w => texts(r.pages[0].prims).find(t => t.text === w)!);
    expect(rt.y).toBeCloseTo(l.y, 3);
    const room = 595.28 - 80 - 10;
    expect(rt.x - l.x).toBeCloseTo(room / 4 + 10, 0);
  });

  it('continues below the tallest column, and lets a long column run over to the next page', () => {
    const long = Array.from({ length: 120 }, (_, i) => text(`line ${i}`));
    const r = lay([{ type: 'columns', columns: [{ items: long }, { items: [text('short')] }] }, text('after')]);
    expect(r.pages.length).toBeGreaterThan(1);
    const last = r.pages[r.pages.length - 1];
    const lastLine = texts(last.prims).filter(t => t.text.startsWith('line')).pop()!;
    const after = texts(last.prims).find(t => t.text === 'after')!;
    expect(after.y).toBeGreaterThan(lastLine.y);
    expect(texts(r.pages[0].prims).some(t => t.text === 'short')).toBe(true);
    expect(texts(r.pages[1].prims).some(t => t.text === 'short')).toBe(false);
  });

  it('draws cards behind the columns, level with each other', () => {
    const r = lay([{ type: 'columns', columns: [{ card: true, items: [text('a'), text('b'), text('c')] }, { card: true, accent: '#e15759', items: [text('x')] }] }]);
    const cards = r.pages[0].prims.filter(p => p.k === 'rect' && p.stroke);
    expect(cards.length).toBe(2);
    expect((cards[0] as { h: number }).h).toBeCloseTo((cards[1] as { h: number }).h, 3);
    expect(r.pages[0].prims.some(p => p.k === 'rect' && p.fill === '#e15759' && p.h === 3)).toBe(true);
  });

  it('a chart and a table in columns still work, with ids that tell them apart', () => {
    const chart = (c: string): BodyItem => ({ type: 'chart', chart: 'pie', dataset: 'sales', category: 'region', series: [{ field: 'amount' }], height: 150, title: c });
    const r = lay([{ type: 'columns', columns: [{ items: [chart('one')] }, { items: [chart('two')] }] }]);
    const ids = new Set(actions(r.pages[0].prims).flatMap(a => (a.type === 'series' ? [String(a.item)] : [])));
    expect([...ids].sort()).toEqual(['0.0/0', '0.1/0']);
  });

  it('gives the items inside columns regions of their own, so the designer can pick them', () => {
    const r = lay([text('top'), { type: 'columns', columns: [{ items: [text('a'), text('b')] }, { items: [text('c')] }] }]);
    const regions = r.pages[0].regions!.filter(x => x.section === 'body');
    expect(regions.filter(x => x.list === 'body.1.columns.0.items').map(x => x.index)).toEqual([0, 1]);
    expect(regions.find(x => x.list === 'body.1.columns.1.items')!.x).toBeGreaterThan(200);
    expect(regions.find(x => x.index === 1 && !x.list)).toBeDefined(); // the columns item as a whole
  });

  it('validates', () => {
    const bad = report([{ type: 'columns', columns: [] }]);
    expect(validateReport(bad).join()).toMatch(/1 to 6 columns/);
    const ok = report([{ type: 'columns', columns: [{ items: [{ type: 'text', text: 'x' }] }] }]);
    expect(validateReport(ok)).toEqual([]);
  });
});

describe('sub-reports', () => {
  const child = (): ReportDef => ({
    ...newReport('Child'), header: [], footer: [],
    parameters: [{ name: 'region', type: 'text' }],
    datasets: [{ name: 'd', sql: 'x' }],
    body: [{ type: 'text', text: 'Region {{region}}: {{Format(Sum(amount), "#,##0")}}', dataset: 'd' }],
  });
  const deps = (calls: string[]) => ({
    load: async (name: string) => (name === 'Child' ? child() : null),
    run: async (_r: ReportDef, params: Record<string, Value>) => {
      calls.push(String(params['region']));
      return { datasets: { d: { columns: ['amount'], rows: [[params['region'] === 'East' ? 35 : 71]] } as DataSet }, warnings: [] };
    },
  });
  type Value = string | number | null | boolean;

  it('runs the other report for each row of a dataset and lays the runs out one after the other', async () => {
    const regions: DataSet = { columns: ['region'], rows: [['East'], ['West']] };
    const parent = report([{ type: 'text', text: 'Top' }, { type: 'subreport', report: 'Child', dataset: 'regions', params: { region: '{{region}}' } }]);
    const calls: string[] = [];
    const res = await resolveSubreports(parent, {}, { regions }, deps(calls));
    expect(calls).toEqual(['East', 'West']);
    expect(Object.keys(res.subs).length).toBe(2);
    const r = layoutReport({ report: parent, params: {}, datasets: { regions }, fonts, subs: res.subs });
    expect(texts(r.pages[0].prims).map(t => t.text)).toEqual(['Top', 'Region East: 35', 'Region West: 71']);
    expect(r.warnings).toEqual([]);
  });

  it('hands the parent\'s same-named parameter down when the item maps nothing', async () => {
    const parent = report([{ type: 'subreport', report: 'Child' }], { parameters: [{ name: 'region', type: 'text' }] });
    const calls: string[] = [];
    const res = await resolveSubreports(parent, { region: 'West' }, {}, deps(calls));
    expect(calls).toEqual(['West']);
    expect(Object.keys(res.subs)).toEqual([subKey('Child', { region: 'West' })]);
  });

  it('says what is missing instead of failing', async () => {
    const parent = report([{ type: 'subreport', report: 'Nope' }]);
    const res = await resolveSubreports(parent, {}, {}, deps([]));
    expect(res.warnings.join()).toMatch(/Nope does not exist/);
    const r = layoutReport({ report: parent, params: {}, datasets: {}, fonts, subs: res.subs });
    expect(texts(r.pages[0].prims)[0].text).toMatch(/not available/);
  });

  it('refuses a report that contains itself, and caps the repeats', async () => {
    const loop: ReportDef = { ...child(), name: 'Loop', body: [{ type: 'subreport', report: 'Loop' }] };
    const res = await resolveSubreports(loop, {}, {}, { load: async () => loop, run: async () => ({ datasets: {}, warnings: [] }) });
    expect(res.warnings.join()).toMatch(/contains itself/);
    const many: DataSet = { columns: ['region'], rows: Array.from({ length: 30 }, (_, i) => [`R${i}`]) };
    const parent = report([{ type: 'subreport', report: 'Child', dataset: 'many', params: { region: '{{region}}' }, limit: 5 }]);
    const calls: string[] = [];
    const capped = await resolveSubreports(parent, {}, { many }, deps(calls));
    expect(calls.length).toBe(5);
    expect(capped.warnings.join()).toMatch(/first 5 of 30/);
  });
});
