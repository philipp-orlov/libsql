// Copyright (c) 2026 Philipp Orlov

import * as zlib from 'node:zlib';
import { ChartData, drawChart, Measurer } from './chart';
import { newReport, ReportDef, validateReport } from './definition';
import { DataSet, layoutReport, ViewState } from './layout';
import { buildPdf } from './pdf';
import { pageToSvg } from './svg';
import { Hit, PageDoc, Prim, TextPrim } from './primitives';
import { testFonts } from './test-fonts';

const fonts = testFonts();
const m: Measurer = { width: (t, f, s) => fonts[f].measure(t, s) };

const data: ChartData = {
  categories: ['East', 'North', 'South', 'West'],
  series: [
    { label: 'Revenue', color: '#4e79a7', values: [100, 250, 175, 50] },
    { label: 'Profit', color: '#f28e2b', values: [30, 90, 60, 10] },
  ],
};
const box = { x: 0, y: 0, w: 420, h: 220 };
const hits = (prims: Prim[], cls: string): Prim[] => prims.filter(p => 'hit' in p && p.hit?.cls === cls);
const hitOf = (p: Prim): Hit => (p as { hit: Hit }).hit;
const texts = (prims: Prim[]): string[] => prims.filter((p): p is TextPrim => p.k === 'text').map(p => p.text);

describe('interactive charts', () => {
  it('give every bar a highlight key and every category a tooltip of all its series', () => {
    const prims = drawChart({ kind: 'bar' }, data, box, { title: 'T' }, m, { item: 3, hidden: [] });
    expect(hits(prims, 'bar').length).toBe(8);
    const slots = hits(prims, 'slot');
    expect(slots.length).toBe(4);
    const north = hitOf(slots[1]);
    expect(north.key).toBe('North');
    expect(north.ghost).toBe(true);
    expect(north.tip!.title).toBe('North');
    expect(north.tip!.rows.map(r => `${r.label}=${r.value}`)).toEqual(['Revenue=250', 'Profit=90']);
    // the slots are drawn last, on top, so that the pointer finds them
    expect(prims.slice(-4).every(p => 'hit' in p && p.hit?.cls === 'slot')).toBe(true);
  });

  it('has a legend whose entries switch a series, and a hidden series leaves the plot', () => {
    const shown = drawChart({ kind: 'bar' }, data, box, {}, m, { item: 2, hidden: [] });
    const entries = hits(shown, 'legend');
    expect(entries.map(e => hitOf(e).action)).toEqual([{ type: 'series', item: 2, index: 0 }, { type: 'series', item: 2, index: 1 }]);
    const hiddenOne = drawChart({ kind: 'bar' }, data, box, {}, m, { item: 2, hidden: [1] });
    expect(hits(hiddenOne, 'bar').length).toBe(4); // only Revenue
    expect(hitOf(hits(hiddenOne, 'slot')[0]).tip!.rows.length).toBe(1);
    expect(hits(hiddenOne, 'legend').length).toBe(2); // the entry stays so that it can be switched back
    expect(texts(hiddenOne)).toContain('Profit');
    // with Profit off the scale fits Revenue alone: a 250 bar is as tall as the plot allows
    const tallest = (prims: Prim[]) => Math.max(...hits(prims, 'bar').map(p => (p as { h: number }).h));
    expect(tallest(hiddenOne)).toBeCloseTo(tallest(shown), 0);
  });

  it('stacks bars on each other and shows the total', () => {
    const prims = drawChart({ kind: 'bar', stacked: true }, data, box, {}, m, { item: 0, hidden: [] });
    const north = hitOf(hits(prims, 'slot')[1]).tip!;
    expect(north.rows[north.rows.length - 1]).toEqual({ label: 'Total', value: '340', bold: true });
    const bars = hits(prims, 'bar') as unknown as { x: number; y: number; h: number }[];
    // North: revenue first, profit piled on top of it at the same x
    expect(bars[2].x).toBeCloseTo(bars[3].x, 3);
    expect(bars[3].y + bars[3].h).toBeCloseTo(bars[2].y, 1);
  });

  it('lays bars down and puts the category names on the left', () => {
    const prims = drawChart({ kind: 'bar', horizontal: true }, data, box, {}, m, { item: 0, hidden: [] });
    const bars = hits(prims, 'bar') as unknown as { x: number; w: number; h: number }[];
    expect(new Set(bars.map(b => Math.round(b.x))).size).toBe(1); // all start at the zero line
    expect(Math.max(...bars.map(b => b.w))).toBeGreaterThan(100);
    const east = prims.find((p): p is TextPrim => p.k === 'text' && p.text === 'East')!;
    expect(east.x).toBeLessThan(60);
  });

  it('draws areas with a translucent fill, lines with points, and labels on request', () => {
    const area = drawChart({ kind: 'area' }, data, box, {}, m, { item: 0, hidden: [] });
    expect(area.some(p => p.k === 'path' && p.fill && p.fillOpacity !== undefined && p.fillOpacity < 1)).toBe(true);
    const line = drawChart({ kind: 'line', labels: true }, data, box, {}, m, { item: 0, hidden: [] });
    expect(hits(line, 'point').length).toBe(8);
    expect(texts(line)).toContain('175'); // not an axis tick: a label on a point
    expect(texts(drawChart({ kind: 'line' }, data, box, {}, m, { item: 0, hidden: [] }))).not.toContain('175');
  });

  it('makes pie slices that pop out and carry their share; the legend switches a slice', () => {
    const prims = drawChart({ kind: 'pie' }, data, box, {}, m, { item: 1, hidden: [] });
    const slices = hits(prims, 'slice');
    expect(slices.length).toBe(4);
    const north = hitOf(slices[1]);
    expect(north.key).toBe('North');
    expect(north.fx).toBeDefined();
    expect(north.tip!.rows.map(r => r.value)).toEqual(['250', '43.5%']);
    const off = drawChart({ kind: 'pie' }, data, box, {}, m, { item: 1, hidden: [1] });
    expect(hits(off, 'slice').length).toBe(3);
    expect(hitOf(hits(off, 'slice')[0]).tip!.rows[1].value).toBe('30.8%'); // the shares are of what is shown
    expect(hits(off, 'legend').length).toBe(4);
  });

  it('draws a donut with the total in its hole, and says when everything is off', () => {
    const donut = drawChart({ kind: 'pie', donut: true }, data, box, {}, m, { item: 0, hidden: [] });
    expect(texts(donut)).toContain('Total');
    expect(texts(donut)).toContain('575');
    expect(texts(drawChart({ kind: 'pie' }, data, box, {}, m, { item: 0, hidden: [0, 1, 2, 3] }))).toContain('Everything is switched off');
  });
});

const sales: DataSet = {
  columns: ['region', 'name', 'amount'],
  rows: [['West', 'w1', 5], ['East', 'e1', 30], ['East', 'e2', 10], ['West', 'w2', 20], ['North', 'n1', 99]],
};
function report(body: ReportDef['body']): ReportDef {
  return { ...newReport('V'), datasets: [{ name: 'sales', sql: 'x' }], body };
}
const grouped = report([
  { type: 'text', text: 'Title' },
  { type: 'table', dataset: 'sales', groups: [{ field: 'region' }], columns: [{ field: 'name', header: 'Name' }, { field: 'amount', header: 'Amount', total: 'sum' }] },
]);
const run = (r: ReportDef, view?: ViewState) => layoutReport({ report: r, params: {}, datasets: { sales }, fonts, view });
const names = (res: ReturnType<typeof run>): string[] => res.pages.flatMap(p => texts(p.prims)).filter(t => /^[a-z]\d$/.test(t));

describe('view state', () => {
  it('sorts a table by the column the reader clicked, inside the groups', () => {
    expect(names(run(grouped))).toEqual(['e1', 'e2', 'n1', 'w1', 'w2']);
    expect(names(run(grouped, { sort: { '1': { column: 1, dir: 'desc' } } }))).toEqual(['e1', 'e2', 'n1', 'w2', 'w1']);
    expect(names(run(grouped, { sort: { '1': { column: 1, dir: 'asc' } } }))).toEqual(['e2', 'e1', 'n1', 'w1', 'w2']);
  });

  it('offers a click on every heading and shows which column is sorted', () => {
    const page = run(grouped).pages[0];
    const heads = page.prims.filter(p => 'hit' in p && p.hit?.cls === 'th');
    expect(heads.map(h => hitOf(h).action)).toEqual([{ type: 'sort', item: 1, column: 0 }, { type: 'sort', item: 1, column: 1 }]);
    const plain = run(grouped).pages[0].prims.filter(p => p.k === 'path').length;
    const sorted = run(grouped, { sort: { '1': { column: 1, dir: 'asc' } } }).pages[0].prims.filter(p => p.k === 'path').length;
    expect(sorted).toBe(plain + 1); // the arrow
  });

  it('folds a group to its heading, which then shows the totals', () => {
    const open = run(grouped);
    const fold = open.pages[0].prims.filter(p => 'hit' in p && p.hit?.cls === 'fold').map(p => hitOf(p).action as { id: string });
    expect(fold.length).toBe(3);
    const eastId = fold[0].id;
    const folded = run(grouped, { folded: [eastId] });
    expect(names(folded)).toEqual(['n1', 'w1', 'w2']);
    expect(texts(folded.pages[0].prims)).toContain('40'); // East's 30 + 10, in its heading
    // the other groups are untouched, and the same id works again after the rows change order
    expect(run(grouped, { folded: ['nonsense'] }).pages[0].prims.length).toBe(open.pages[0].prims.length);
  });

  it('hides the series the reader switched off, in the chart and in what the PDF draws', () => {
    const r = report([{ type: 'chart', chart: 'bar', dataset: 'sales', category: 'name', series: [{ field: 'amount' }, { field: 'amount', label: 'Again' }], height: 160 }]);
    const all = run(r).pages[0].prims.filter(p => 'hit' in p && p.hit?.cls === 'bar').length;
    const one = run(r, { hidden: { '0': [1] } }).pages[0].prims.filter(p => 'hit' in p && p.hit?.cls === 'bar').length;
    expect(all).toBe(10);
    expect(one).toBe(5);
  });
});

describe('regions', () => {
  it('say where each item is, so that a click selects it, on every page it spans', () => {
    const big: DataSet = { columns: ['region', 'name', 'amount'], rows: Array.from({ length: 150 }, (_, i) => ['R', `x${i}`, i]) };
    const r = report([{ type: 'text', text: 'Title' }, { type: 'table', dataset: 'sales', columns: [{ field: 'name' }, { field: 'amount' }] }, { type: 'text', text: 'After' }]);
    const res = layoutReport({ report: r, params: {}, datasets: { sales: big }, fonts });
    const of = (page: PageDoc, section: string, index: number) => (page.regions ?? []).filter(x => x.section === section && x.index === index);
    expect(res.pages.length).toBeGreaterThan(2);
    expect(of(res.pages[0], 'body', 0).length).toBe(1);
    for (const page of res.pages.slice(0, -1)) expect(of(page, 'body', 1).length).toBe(1); // the table fills pages
    const lastPage = res.pages[res.pages.length - 1];
    expect(of(lastPage, 'body', 2).length).toBe(1);
    const header = of(res.pages[0], 'header', 0)[0];
    expect(header.y).toBeLessThan(of(res.pages[0], 'body', 0)[0].y);
    expect(of(res.pages[0], 'footer', 1).length).toBe(1);
    // regions do not overlap
    const body = (res.pages[0].regions ?? []).filter(x => x.section === 'body').sort((a, b) => a.y - b.y);
    for (let i = 1; i < body.length; i++) expect(body[i].y).toBeGreaterThanOrEqual(body[i - 1].y + body[i - 1].h - 0.5);
  });
});

describe('the preview and the PDF', () => {
  const r = report([{ type: 'chart', chart: 'area', dataset: 'sales', category: 'name', series: [{ field: 'amount' }], height: 160, title: 'A "quoted" <title>' }]);
  const laid = run(r);

  it('writes the interactivity into the SVG, escaped', () => {
    const svg = pageToSvg(laid.pages[0]);
    expect(svg).toContain('data-tip="{&quot;title&quot;:&quot;w1&quot;');
    expect(svg).toContain('class="slot"');
    expect(svg).toContain('pointer-events="all"');
    expect(svg).toContain('fill-opacity="0.22"');
    expect(svg).toContain('A &quot;quoted&quot; &lt;title&gt;');
    expect(svg).not.toMatch(/data-tip="[^"]*"[^>]*"[^"]*"\s/); // no unescaped quote broke an attribute
    expect(pageToSvg({ width: 10, height: 10, prims: [{ k: 'path', cmds: [['M', 0, 0], ['L', 5, 5], ['Z']], fill: '#f00', hit: { cls: 'slice', fx: { dx: 2, dy: -3 }, key: 'k&' } }] })).toContain('style="--dx:2px;--dy:-3px"');
  });

  it('leaves the ghosts out of the PDF and gives translucent fills a graphics state', async () => {
    const pdf = await buildPdf(laid.pages, fonts);
    const text = Buffer.from(pdf).toString('latin1');
    expect(text).toContain('/ExtGState');
    expect(text).toMatch(/\/Type \/ExtGState \/ca 0\.22 \/CA 0\.22/);
    const streams = [...text.matchAll(/stream\n([\s\S]*?)\nendstream/g)].map(x => { try { return zlib.inflateSync(Buffer.from(x[1], 'latin1')).toString('latin1'); } catch { return ''; } });
    const content = streams.find(s => s.includes(' Tf '))!;
    expect(content).toContain('/GS1 gs');
    // the number of filled rectangles equals the number of non-ghost rectangles
    const drawn = laid.pages[0].prims.filter(p => p.k === 'rect' && !(p.hit?.ghost)).length;
    expect((content.match(/ re /g) ?? []).length).toBe(drawn);
  });
});

describe('definitions', () => {
  it('lets a dataset have a name like customers-data, but not a parameter', () => {
    const r = { ...newReport('V'), datasets: [{ name: 'customers-data', sql: 'SELECT 1' }, { name: 'My data', sql: 'SELECT 2' }], body: [] };
    expect(validateReport(r)).toEqual([]);
    expect(validateReport({ ...r, datasets: [{ name: '1bad', sql: 'x' }] }).join()).toMatch(/dataset name/);
    expect(validateReport({ ...r, parameters: [{ name: 'a-b', type: 'text' }] }).join()).toMatch(/not an identifier/);
  });
  it('knows the chart kinds', () => {
    const r = { ...report([{ type: 'chart', chart: 'radar' as 'bar', dataset: 'sales', category: 'name', series: [{ field: 'amount' }], height: 100 }]) };
    expect(validateReport(r).join()).toMatch(/unknown kind radar/);
  });
});
