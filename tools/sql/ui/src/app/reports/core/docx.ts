// Copyright (c) 2026 Philipp Orlov

// A report as a Word document (.docx, written by hand): paragraphs with their bold, colors and sizes, real tables for
// tables and cross-tabs (heading row repeated on each page, the cross-tab's colors in the cells), charts as pictures,
// pictures, columns as a borderless table, sub-reports in place. Free of Angular imports on purpose.

import { Block } from './blocks';
import { ChartData } from './chart';
import { pageDimensions, ReportDef } from './definition';
import { formatValue, Value } from './format';
import { heatFill, inkOn } from './matrix';
import { Paragraph } from './richtext';
import { effective } from './richtext';
import { buildZip } from './zip';

type ChartBlock = Extract<Block, { t: 'chart' }>;

export interface DocxOptions {
  now?: Date;
  /** PNG bytes for a picture Word cannot show as it is (SVG, WebP); `width` x `height` in pixels. */
  rasterize?: (src: string, width: number, height: number) => Promise<Uint8Array | null>;
  /** A chart as a PNG that is `width` points wide; the size is the size on the page, in points. */
  chart?: (block: ChartBlock, width: number) => Promise<{ png: Uint8Array; width: number; height: number } | null>;
}

export interface DocumentInfo {
  report: ReportDef;
  params: Record<string, Value>;
  blocks: Block[];
}

const XML = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n';
const W = 'http://schemas.openxmlformats.org/wordprocessingml/2006/main';
const REL = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships';
const FONT = 'Arial';
const SLATE = '37474F';

const esc = (s: string): string => s.replace(/[\u0000-\u0008\u000B\u000C\u000E-\u001F]/g, '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const hex = (c: string | undefined, fallback = '000000'): string => {
  if (!c) return fallback;
  const h = c.replace('#', '');
  return (h.length === 3 ? [...h].map(x => x + x).join('') : h).toUpperCase();
};
const tw = (pt: number): number => Math.round(pt * 20);
const emu = (pt: number): number => Math.round(pt * 12700);

interface Media {
  rid: string;
  file: string;
  bytes: Uint8Array;
  /** Points. */
  width: number;
  height: number;
}

interface RunLook {
  face: 'regular' | 'bold';
  size: number;
  color: string;
  underline?: boolean;
  bg?: string;
}

const run = (text: string, l: RunLook): string =>
  `<w:r><w:rPr><w:rFonts w:ascii="${FONT}" w:hAnsi="${FONT}" w:cs="${FONT}"/>${l.face === 'bold' ? '<w:b/>' : ''}<w:color w:val="${hex(l.color)}"/><w:sz w:val="${Math.round(l.size * 2)}"/>${l.underline ? '<w:u w:val="single"/>' : ''}${l.bg ? `<w:shd w:val="clear" w:color="auto" w:fill="${hex(l.bg)}"/>` : ''}</w:rPr><w:t xml:space="preserve">${esc(text)}</w:t></w:r>`;

interface ParaOptions {
  align?: 'left' | 'center' | 'right';
  fill?: string;
  after?: number;
  before?: number;
  hanging?: boolean;
  /** Left indent in twips. */
  indent?: number;
  keepNext?: boolean;
  border?: boolean;
  /** An exact line height in points (spacers). */
  exact?: number;
}

const para = (content: string, o: ParaOptions = {}): string =>
  `<w:p><w:pPr>${o.keepNext ? '<w:keepNext/>' : ''}${o.border ? '<w:pBdr><w:bottom w:val="single" w:sz="6" w:space="1" w:color="9E9E9E"/></w:pBdr>' : ''}${o.fill ? `<w:shd w:val="clear" w:color="auto" w:fill="${hex(o.fill)}"/>` : ''}` +
  `<w:spacing w:before="${tw(o.before ?? 0)}" w:after="${tw(o.after ?? 2)}"${o.exact ? ` w:line="${tw(o.exact)}" w:lineRule="exact"` : ''}/>` +
  `${o.hanging ? '<w:ind w:left="360" w:hanging="260"/>' : o.indent ? `<w:ind w:left="${o.indent}"/>` : ''}${o.align && o.align !== 'left' ? `<w:jc w:val="${o.align === 'center' ? 'center' : 'right'}"/>` : ''}</w:pPr>${content}</w:p>`;

function paragraphs(ps: Paragraph[], look: { face: 'regular' | 'bold'; size: number; color: string; align: 'left' | 'center' | 'right'; background?: string }): string {
  return ps
    .map(p => {
      const runs = p.runs.map(r => {
        const e = effective({ face: look.face, size: look.size, color: look.color }, r.style);
        return run(r.text, e);
      });
      const bullet = p.bullet ? run('•\t', { face: 'bold', size: look.size, color: look.color }) : '';
      const biggest = Math.max(look.size, ...p.runs.map(r => r.style.size ?? 0));
      return para(bullet + (runs.join('') || run('', { face: 'regular', size: biggest, color: look.color })), { align: look.align, fill: look.background, hanging: p.bullet });
    })
    .join('');
}

interface CellOptions {
  fill?: string;
  span?: number;
  top?: { color: string; size: number };
  borders?: string;
  valign?: 'top' | 'center';
}

const cell = (width: number, content: string, o: CellOptions = {}): string =>
  `<w:tc><w:tcPr><w:tcW w:w="${width}" w:type="dxa"/>${o.span ? `<w:gridSpan w:val="${o.span}"/>` : ''}` +
  `${o.borders || o.top ? `<w:tcBorders>${o.top ? `<w:top w:val="single" w:sz="${o.top.size}" w:space="0" w:color="${o.top.color}"/>` : ''}${o.borders ?? ''}</w:tcBorders>` : ''}` +
  `${o.fill ? `<w:shd w:val="clear" w:color="auto" w:fill="${o.fill}"/>` : ''}<w:vAlign w:val="${o.valign ?? 'center'}"/></w:tcPr>${content || '<w:p/>'}</w:tc>`;

const row = (cells: string, o: { header?: boolean; height?: number } = {}): string =>
  `<w:tr><w:trPr><w:cantSplit/>${o.height ? `<w:trHeight w:val="${tw(o.height)}" w:hRule="atLeast"/>` : ''}${o.header ? '<w:tblHeader/>' : ''}</w:trPr>${cells}</w:tr>`;

const table = (widths: number[], rows: string, o: { lines?: boolean; margins?: number } = {}): string =>
  `<w:tbl><w:tblPr><w:tblW w:w="${widths.reduce((a, b) => a + b, 0)}" w:type="dxa"/>` +
  `<w:tblBorders>${o.lines === false ? '' : '<w:top w:val="single" w:sz="4" w:space="0" w:color="D1D5DB"/><w:bottom w:val="single" w:sz="4" w:space="0" w:color="D1D5DB"/><w:insideH w:val="single" w:sz="2" w:space="0" w:color="E5E7EB"/>'}</w:tblBorders>` +
  `<w:tblLayout w:type="fixed"/><w:tblCellMar><w:top w:w="30" w:type="dxa"/><w:left w:w="${o.margins ?? 80}" w:type="dxa"/><w:bottom w:w="30" w:type="dxa"/><w:right w:w="${o.margins ?? 80}" w:type="dxa"/></w:tblCellMar></w:tblPr>` +
  `<w:tblGrid>${widths.map(w => `<w:gridCol w:w="${w}"/>`).join('')}</w:tblGrid>${rows}</w:tbl>`;

/** The widths of `weights` summing to `total`, whole twips. */
function split(total: number, weights: number[]): number[] {
  const sum = weights.reduce((a, b) => a + b, 0) || 1;
  const out = weights.map(w => Math.floor((total * w) / sum));
  out[out.length - 1] += total - out.reduce((a, b) => a + b, 0);
  return out;
}

function chartTable(title: string, data: ChartData, fmt: string, width: number): string {
  const widths = split(width, [2.4, ...data.series.map(() => 1)]);
  const head = row(
    [cell(widths[0], para(run('', { face: 'bold', size: 9, color: 'FFFFFF' })), { fill: SLATE }), ...data.series.map((s, i) => cell(widths[i + 1], para(run(s.label, { face: 'bold', size: 9, color: 'FFFFFF' }), { align: 'right' }), { fill: SLATE }))].join(''),
    { header: true },
  );
  const rows = data.categories.map((c, k) => row([cell(widths[0], para(run(c, { face: 'regular', size: 9, color: '222222' }))), ...data.series.map((s, i) => cell(widths[i + 1], para(run(s.values[k] === null ? '' : formatValue(s.values[k], fmt), { face: 'regular', size: 9, color: '222222' }), { align: 'right' })))].join('')));
  void title;
  return table(widths, head + rows.join(''));
}

export async function buildDocx(info: DocumentInfo, options: DocxOptions = {}): Promise<Uint8Array> {
  const { report } = info;
  const now = options.now ?? new Date();
  const [pw, ph] = pageDimensions(report.page);
  const mg = report.page.margins;
  const contentPt = pw - mg.left - mg.right;
  const media = new Map<object, Media>();
  const files: { name: string; data: Uint8Array }[] = [];
  const extension = new Set<string>();

  const addMedia = (key: object, bytes: Uint8Array, ext: string, width: number, height: number): void => {
    const file = `image${files.length + 1}.${ext}`;
    files.push({ name: `word/media/${file}`, data: bytes });
    extension.add(ext);
    media.set(key, { rid: `rIdM${files.length}`, file, bytes, width, height });
  };

  // pictures and charts are made before the document is written, because that is asynchronous
  const prepare = async (blocks: Block[], widthPt: number): Promise<void> => {
    for (const b of blocks) {
      if (b.t === 'image') {
        const k = b.info.kind;
        if (k === 'png' || k === 'jpeg' || k === 'gif') addMedia(b, b.info.bytes, k === 'jpeg' ? 'jpeg' : k, b.w, b.h);
        else {
          const png = await options.rasterize?.(b.src, Math.round(b.w * 3), Math.round(b.h * 3));
          if (png) addMedia(b, png, 'png', b.w, b.h);
        }
      } else if (b.t === 'chart') {
        const made = await options.chart?.(b, widthPt);
        if (made) addMedia(b, made.png, 'png', made.width, made.height);
      } else if (b.t === 'columns') {
        const gap = b.gap;
        const room = widthPt - gap * (b.cells.length - 1);
        const sum = b.cells.reduce((a, c) => a + c.width, 0) || 1;
        for (const c of b.cells) await prepare(c.blocks, (room * c.width) / sum - (c.card ? 12 : 0));
      }
    }
  };
  await prepare(info.blocks, contentPt);

  let drawing = 0;
  const picture = (m: Media, widthPt: number, align: 'left' | 'center' | 'right'): string => {
    let w = m.width;
    let h = m.height;
    if (w > widthPt) {
      h *= widthPt / w;
      w = widthPt;
    }
    const id = ++drawing;
    return para(
      `<w:r><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0"><wp:extent cx="${emu(w)}" cy="${emu(h)}"/><wp:docPr id="${id}" name="Picture ${id}"/><wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect="1"/></wp:cNvGraphicFramePr>` +
        `<a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture"><pic:pic><pic:nvPicPr><pic:cNvPr id="${id}" name="${m.file}"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed="${m.rid}"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>` +
        `<pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="${emu(w)}" cy="${emu(h)}"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>`,
      { align, after: 4 },
    );
  };

  const heading = (text: string): string => para(run(text, { face: 'bold', size: 11, color: '263238' }), { keepNext: true, before: 4, after: 3 });
  const gapAfterTable = para('', { exact: 6, after: 0 });

  const render = (blocks: Block[], width: number): string => {
    let out = '';
    for (const b of blocks) {
      switch (b.t) {
        case 'text':
          out += paragraphs(b.paragraphs, b.look);
          break;
        case 'rule':
          out += para('', { border: true, exact: 6, after: 2 });
          break;
        case 'space':
          out += para('', { exact: Math.max(b.h, 1), after: 0 });
          break;
        case 'break':
          out += '<w:p><w:r><w:br w:type="page"/></w:r></w:p>';
          break;
        case 'image': {
          const m = media.get(b);
          if (m) {
            out += picture(m, Math.min(b.w, width / 20), b.align);
            if (b.caption) out += para(run(b.caption, { face: 'regular', size: 8.5, color: '6B7280' }), { align: b.align });
          }
          break;
        }
        case 'chart': {
          const m = media.get(b);
          if (b.item.title) out += heading(b.item.title);
          out += m ? picture(m, width / 20, 'left') : chartTable(b.title, b.data, b.item.format ?? '#,##0.##', width) + gapAfterTable;
          break;
        }
        case 'table': {
          const cols = b.model.columns;
          const widths = split(width, cols.map(c => c.def.width ?? 1));
          const size = b.look.size;
          const ink = b.look.color;
          let rows = row(cols.map((c, i) => cell(widths[i], para(run(c.header, { face: 'bold', size, color: 'FFFFFF' }), { align: c.align }), { fill: SLATE })).join(''), { header: true });
          let zebra = 0;
          for (const r of b.model.rows) {
            const group = r.kind === 'group';
            const total = r.kind === 'total' || r.kind === 'grand';
            const fill = group || total ? 'F1F3F5' : b.item.zebra && zebra++ % 2 ? 'FAFAFA' : undefined;
            rows += row(
              r.cells
                .map((c, i) => cell(widths[i], para(run(c.text, { face: group || total ? 'bold' : b.look.face, size, color: ink }), { align: c.align, after: 0, indent: i === 0 && r.level ? r.level * 160 : undefined }), { fill, top: r.kind === 'grand' ? { color: '6B7280', size: 12 } : r.kind === 'total' ? { color: '9CA3AF', size: 6 } : undefined }))
                .join(''),
            );
          }
          out += table(widths, rows) + gapAfterTable;
          break;
        }
        case 'matrix': {
          const m = b.model;
          const size = b.item.style?.size ?? 9;
          const colWeights = [Math.max(2, Math.min(4, Math.max(...m.rows.map(r => r.label.length), m.corner.length) / 8)), ...m.columns.map(() => 1), ...(b.item.rowTotals ? [1] : [])];
          const widths = split(width, colWeights);
          const fmt = b.item.format ?? '#,##0.##';
          let rows = row(
            [cell(widths[0], para(run(m.corner, { face: 'bold', size: size - 0.5, color: 'FFFFFF' })), { fill: SLATE }), ...[...m.columns.map(c => c.label), ...(b.item.rowTotals ? ['Total'] : [])].map((h, i) => cell(widths[i + 1], para(run(h, { face: 'bold', size: size - 0.5, color: 'FFFFFF' }), { align: 'center' }), { fill: SLATE }))].join(''),
            { header: true },
          );
          for (const r of m.rows) {
            const bold = r.kind !== 'data';
            const fillRow = bold ? 'F1F3F5' : undefined;
            const values = [...r.values, ...(b.item.rowTotals ? [r.total] : [])];
            rows += row(
              [
                cell(widths[0], para(run(r.label, { face: bold ? 'bold' : 'regular', size, color: '222222' }), { after: 0, indent: r.level ? 200 : undefined }), { fill: fillRow, top: r.kind === 'total' ? { color: '6B7280', size: 8 } : undefined }),
                ...values.map((v, i) => {
                  const isTotal = i >= m.columns.length;
                  const heat = r.kind === 'data' && !isTotal && v !== null ? heatFill(b.item.heat, v, m.min, m.max) : undefined;
                  return cell(widths[i + 1], para(run(v === null || (r.kind === 'group' && !b.item.subtotals) ? '' : formatValue(v, fmt), { face: bold || isTotal ? 'bold' : 'regular', size, color: hex(inkOn(heat)) }), { align: 'center', after: 0 }), {
                    fill: heat ? hex(heat) : isTotal && r.kind === 'data' ? 'F1F3F5' : fillRow,
                    top: r.kind === 'total' ? { color: '6B7280', size: 8 } : undefined,
                  });
                }),
              ].join(''),
            );
          }
          out += (b.item.title ? heading(b.item.title) : '') + table(widths, rows, { lines: false }) + gapAfterTable;
          break;
        }
        case 'columns': {
          const gap = tw(b.gap);
          const inner = width - gap * (b.cells.length - 1);
          const cw = split(inner, b.cells.map(c => c.width));
          const grid: number[] = [];
          const cells: string[] = [];
          b.cells.forEach((c, i) => {
            if (i) {
              grid.push(gap);
              cells.push(cell(gap, '<w:p/>'));
            }
            grid.push(cw[i]);
            const pad = c.card ? 120 : 0;
            const content = render(c.blocks, cw[i] - 2 * pad);
            const line = '<w:left w:val="single" w:sz="4" w:space="0" w:color="E0E0E0"/><w:bottom w:val="single" w:sz="4" w:space="0" w:color="E0E0E0"/><w:right w:val="single" w:sz="4" w:space="0" w:color="E0E0E0"/>';
            const body = c.card ? table([cw[i] - 2 * pad], row(cell(cw[i] - 2 * pad, content || '<w:p/>')), { lines: false, margins: 0 }) : content;
            cells.push(
              cell(cw[i], c.card ? `<w:p><w:pPr><w:spacing w:before="0" w:after="0" w:line="${tw(2)}" w:lineRule="exact"/></w:pPr></w:p>` + body : content, {
                valign: 'top',
                fill: c.card ? hex(c.background, 'FFFFFF') : undefined,
                top: c.card ? { color: hex(c.accent, 'E0E0E0'), size: c.accent ? 24 : 4 } : undefined,
                borders: c.card ? line : undefined,
              }),
            );
          });
          out += table(grid, row(cells.join('')), { lines: false, margins: 100 }) + gapAfterTable;
          break;
        }
      }
    }
    return out;
  };

  const widthTw = tw(contentPt);
  const title = report.title || report.name;
  const mediaTypes: Record<string, string> = { png: 'image/png', jpeg: 'image/jpeg', gif: 'image/gif' };
  const body = render(info.blocks, widthTw);
  const field = (code: string, shown: string): string =>
    `<w:r><w:fldChar w:fldCharType="begin"/></w:r><w:r><w:instrText xml:space="preserve"> ${code} </w:instrText></w:r><w:r><w:fldChar w:fldCharType="separate"/></w:r>${run(shown, { face: 'regular', size: 8.5, color: '6B7280' })}<w:r><w:fldChar w:fldCharType="end"/></w:r>`;
  const part = (tag: 'hdr' | 'ftr', inner: string): string => XML + `<w:${tag} xmlns:w="${W}" xmlns:r="${REL}">${inner}</w:${tag}>`;

  const entries = [
    {
      name: '[Content_Types].xml',
      data: XML + '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/>' +
        [...extension].map(e => `<Default Extension="${e}" ContentType="${mediaTypes[e]}"/>`).join('') +
        '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>' +
        '<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>' +
        '<Override PartName="/word/header1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml"/>' +
        '<Override PartName="/word/footer1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml"/>' +
        '<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/></Types>',
    },
    {
      name: '_rels/.rels',
      data: XML + `<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="${REL}/officeDocument" Target="word/document.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/></Relationships>`,
    },
    {
      name: 'docProps/core.xml',
      data: XML + `<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><dc:title>${esc(title)}</dc:title><dc:creator>libsql sql tool</dc:creator><dcterms:created xsi:type="dcterms:W3CDTF">${now.toISOString().replace(/\.\d+Z$/, 'Z')}</dcterms:created></cp:coreProperties>`,
    },
    {
      name: 'word/_rels/document.xml.rels',
      data: XML + `<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rIdS" Type="${REL}/styles" Target="styles.xml"/><Relationship Id="rIdH" Type="${REL}/header" Target="header1.xml"/><Relationship Id="rIdF" Type="${REL}/footer" Target="footer1.xml"/>` +
        [...media.values()].map(m => `<Relationship Id="${m.rid}" Type="${REL}/image" Target="media/${m.file}"/>`).join('') + '</Relationships>',
    },
    {
      name: 'word/styles.xml',
      data: XML + `<w:styles xmlns:w="${W}"><w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii="${FONT}" w:hAnsi="${FONT}" w:cs="${FONT}"/><w:sz w:val="20"/><w:szCs w:val="20"/><w:lang w:val="en-US"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after="40" w:line="252" w:lineRule="auto"/></w:pPr></w:pPrDefault></w:docDefaults><w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/></w:style></w:styles>`,
    },
    { name: 'word/header1.xml', data: part('hdr', para(run(title, { face: 'bold', size: 8.5, color: '6B7280' }), { border: true })) },
    { name: 'word/footer1.xml', data: part('ftr', para(run('Page ', { face: 'regular', size: 8.5, color: '6B7280' }) + field('PAGE', '1') + run(' of ', { face: 'regular', size: 8.5, color: '6B7280' }) + field('NUMPAGES', '1'), { align: 'right' })) },
    {
      name: 'word/document.xml',
      data: XML + `<w:document xmlns:w="${W}" xmlns:r="${REL}" xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture"><w:body>${body}${para('')}` +
        `<w:sectPr><w:headerReference w:type="default" r:id="rIdH"/><w:footerReference w:type="default" r:id="rIdF"/><w:pgSz w:w="${tw(pw)}" w:h="${tw(ph)}"${report.page.orientation === 'landscape' ? ' w:orient="landscape"' : ''}/>` +
        `<w:pgMar w:top="${tw(mg.top)}" w:right="${tw(mg.right)}" w:bottom="${tw(mg.bottom)}" w:left="${tw(mg.left)}" w:header="${tw(Math.max(mg.top / 2, 18))}" w:footer="${tw(Math.max(mg.bottom / 2, 18))}" w:gutter="0"/></w:sectPr></w:body></w:document>`,
    },
    ...files,
  ];
  return buildZip(entries, now);
}
