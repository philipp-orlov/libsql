// Copyright (c) 2026 Philipp Orlov

// A report as content instead of pages: text with its runs, tables and cross-tabs as rows of cells, charts as their data,
// pictures, columns and sub-reports in place. This is what the CSV, Excel and Word exports write (they cannot use the laid-out
// pages: a spreadsheet wants numbers, a document wants paragraphs and tables). Free of Angular imports on purpose.

import { ChartData, chartData } from './chart';
import { BodyItem, ChartItem, defaultParams, ImageItem, MatrixItem, ReportDef, Style, TableItem } from './definition';
import { interpolate } from './expr';
import { Value } from './format';
import { imageInfo, ImageInfo } from './image';
import { matrixModel, MatrixModel } from './matrix';
import { Paragraph, parseRich } from './richtext';
import { context, DataSet, Env } from './scope';
import { subCalls, SubRuns, subKey } from './subreport';
import { tableModel, TableModel } from './tablemodel';

export interface TextLook {
  face: 'regular' | 'bold';
  size: number;
  color: string;
  align: 'left' | 'center' | 'right';
  background?: string;
}

export type Block =
  | { t: 'text'; paragraphs: Paragraph[]; look: TextLook }
  | { t: 'table'; id: string; title: string; item: TableItem; model: TableModel; look: TextLook }
  | { t: 'matrix'; id: string; title: string; item: MatrixItem; model: MatrixModel }
  | { t: 'chart'; id: string; title: string; item: ChartItem; data: ChartData; hidden: number[] }
  | { t: 'image'; src: string; info: ImageInfo; /** points */ w: number; h: number; align: 'left' | 'center' | 'right'; caption: string }
  | { t: 'columns'; gap: number; cells: { width: number; blocks: Block[]; card: boolean; accent?: string; background?: string }[] }
  | { t: 'rule' }
  | { t: 'space'; h: number }
  | { t: 'break' };

export interface BlocksInput {
  report: ReportDef;
  params: Record<string, Value>;
  datasets: Record<string, DataSet>;
  subs?: SubRuns;
  view?: { hidden?: Record<string, number[]>; sort?: Record<string, { column: number; dir: 'asc' | 'desc' }> };
  now?: Date;
}

const LOOK: Omit<TextLook, 'background'> = { face: 'regular', size: 10, color: '#000000', align: 'left' };
export const lookOf = (s?: Style): TextLook => ({ ...LOOK, ...(s ?? {}) });

export function buildBlocks(input: BlocksInput): { blocks: Block[]; warnings: string[] } {
  const warnings: string[] = [];
  const warn = (w: string): void => void (warnings.includes(w) || warnings.push(w));
  const now = input.now ?? new Date();

  const items = (list: BodyItem[], report: ReportDef, env: Env, datasets: Record<string, DataSet>, subs: SubRuns, prefix: string, label: string): Block[] => {
    const out: Block[] = [];
    list.forEach((it, i) => {
      const id = prefix === '' ? String(i) : `${prefix}/${i}`;
      const lab = label === '' ? String(i + 1) : `${label}.${i + 1}`;
      switch (it.type) {
        case 'text': {
          const ds = it.dataset ? datasets[it.dataset] ?? null : null;
          const cx = context(env, ds, ds?.rows ?? [], null);
          const paragraphs = it.rich
            ? parseRich(it.text, expr => interpolate('{{' + expr + '}}', cx))
            : interpolate(it.text, cx).split(/\r?\n/).map(line => ({ runs: line ? [{ text: line, style: {} }] : [], bullet: false }));
          out.push({ t: 'text', paragraphs, look: lookOf(it.style) });
          break;
        }
        case 'table': {
          const ds = datasets[it.dataset];
          if (!ds) {
            warn(`table ${lab}: dataset ${it.dataset} has no data`);
            break;
          }
          const model = tableModel(it, ds, env, { sort: input.view?.sort?.[id] }, id, `table ${lab}`);
          model.warnings.forEach(warn);
          out.push({ t: 'table', id, title: `Table ${lab} (${it.dataset})`, item: it, model, look: lookOf(it.style) });
          break;
        }
        case 'matrix': {
          const ds = datasets[it.dataset];
          if (!ds) {
            warn(`matrix ${lab}: dataset ${it.dataset} has no data`);
            break;
          }
          const model = matrixModel(it, ds, `matrix ${lab}`, 1000, input.view?.sort?.[id]);
          model.warnings.forEach(warn);
          out.push({ t: 'matrix', id, title: it.title ? `${it.title}` : `Matrix ${lab} (${it.dataset})`, item: it, model });
          break;
        }
        case 'chart': {
          const ds = datasets[it.dataset];
          if (!ds) {
            warn(`chart ${lab}: dataset ${it.dataset} has no data`);
            break;
          }
          const drawn = chartData(it, ds, `chart ${lab}`, warn);
          if (drawn) out.push({ t: 'chart', id, title: it.title ? it.title : `Chart ${lab} (${it.dataset})`, item: it, data: drawn.data, hidden: input.view?.hidden?.[id] ?? [] });
          break;
        }
        case 'image': {
          const b = imageBlock(it, env, datasets);
          if (b) out.push(b);
          break;
        }
        case 'columns':
          out.push({
            t: 'columns',
            gap: it.gap ?? 12,
            cells: (it.columns ?? []).map((c, k) => ({ width: c.width ?? 1, blocks: items(c.items ?? [], report, env, datasets, subs, `${id}.${k}`, lab), card: !!c.card, accent: c.accent, background: c.background })),
          });
          break;
        case 'subreport': {
          const target = Object.values(subs).find(r => r.report.name === it.report)?.report;
          if (!target) {
            warn(`sub-report ${it.report} is not available`);
            break;
          }
          subCalls(it, target, env, datasets).forEach((call, k) => {
            const run = subs[subKey(it.report, call.params)];
            if (!run) return;
            if (k > 0) out.push(it.pageBreak ? { t: 'break' } : { t: 'space', h: 10 });
            const inner: Env = Object.create(env, { report: { value: run.report }, params: { value: run.params } }) as Env;
            out.push(...items(run.report.body, run.report, inner, run.datasets, run.subs, `${id}~${k}`, `${lab}.${k + 1}`));
          });
          break;
        }
        case 'rule': out.push({ t: 'rule' }); break;
        case 'spacer': out.push({ t: 'space', h: it.height }); break;
        case 'pageBreak': out.push({ t: 'break' }); break;
      }
    });
    return out;
  };

  const imageBlock = (it: ImageItem, env: Env, datasets: Record<string, DataSet>): Block | null => {
    const ds = it.dataset ? datasets[it.dataset] ?? null : null;
    const cx = context(env, ds, ds?.rows ?? [], null);
    const src = interpolate(it.src, cx).trim();
    if (!src) return null;
    const info = imageInfo(src);
    if (!info) {
      warn('an image is not a PNG, JPEG, GIF, WebP or SVG that this report can show');
      return null;
    }
    const ratio = info.width / info.height;
    let w = info.width * 0.75;
    let h = info.height * 0.75;
    if (it.width && it.height) {
      w = it.width;
      h = it.height;
    } else if (it.width) {
      w = it.width;
      h = w / ratio;
    } else if (it.height) {
      h = it.height;
      w = h * ratio;
    }
    return { t: 'image', src, info, w, h, align: it.align ?? 'left', caption: it.caption ? interpolate(it.caption, cx) : '' };
  };

  const env: Env = { report: input.report, params: input.params, now, page: 1, pageCount: 0 };
  const blocks = items(input.report.body, input.report, env, input.datasets, input.subs ?? {}, '', '');
  return { blocks, warnings };
}

/** Every block, nested ones too, in reading order. */
export function walkBlocks(blocks: Block[], visit: (b: Block) => void): void {
  for (const b of blocks) {
    visit(b);
    if (b.t === 'columns') for (const c of b.cells) walkBlocks(c.blocks, visit);
  }
}

export { defaultParams };
