// Copyright (c) 2026 Philipp Orlov

import { Injectable, inject, signal } from '@angular/core';

import { ErrorCode, ParamType, SocketError, SqlSocket } from '../remote';
import { DatabaseService, SOCKET_PATH } from '../services/database.service';
import {
  bindNames,
  buildBlocks,
  buildDocx,
  buildPdf,
  buildXlsx,
  ChartSpec,
  DataSet,
  drawChart,
  FontFace,
  layoutReport,
  LayoutResult,
  newReport,
  ViewState,
  ParamDef,
  ParamOption,
  parseReport,
  ReportDef,
  resolveSubreports,
  SubRuns,
  TrueTypeFont,
  Value,
} from './core';
import { primsToPng, rasterize, rasterizePng } from './raster';

export interface ReportListEntry {
  name: string;
  size: number;
  modified: number;
}

/** What the server said, or why it could not be asked. */
export class ReportError extends Error {
  constructor(
    message: string,
    readonly status: number,
  ) {
    super(message);
  }
}

export type ParamValues = Record<string, Value>;

export interface RunResult {
  datasets: Record<string, DataSet>;
  /** The runs of the sub-reports the report holds. */
  subs: SubRuns;
  warnings: string[];
}

const MAX_ROWS = 100_000;
const FONT_FILES: Record<FontFace, string> = { regular: '/fonts/LiberationSans-Regular.ttf', bold: '/fonts/LiberationSans-Bold.ttf' };
export const FONT_FAMILY = 'ReportSans';

/**
 * Everything the report features need from the server and the browser: the report files (REST), the font the pages are
 * measured, drawn and exported with, a read-only SQL session for the datasets, and the PDF.
 */
@Injectable({ providedIn: 'root' })
export class ReportService {
  private readonly db = inject(DatabaseService);

  /** Set once the fonts are loaded. */
  readonly fontsReady = signal(false);
  /**
   * Whether this server has the report features (a build without SQL_WITH_REPORTS has no /api/v1/reports): null until asked.
   * The page itself is the same either way, so it asks and hides what is not there.
   */
  readonly available = signal<boolean | null>(null);
  /** Grows when a report is saved or deleted, so that lists reload. */
  readonly revision = signal(0);
  private fonts: Promise<Record<FontFace, TrueTypeFont>> | null = null;
  private socket: SqlSocket | null = null;
  private connecting: Promise<SqlSocket> | null = null;

  // ---- report files --------------------------------------------------------------------------------------------------

  private headers(extra: Record<string, string> = {}): Record<string, string> {
    const token = this.db.token;
    return token ? { Authorization: `Bearer ${token}`, ...extra } : extra;
  }

  private async request(method: string, name: string, body?: string): Promise<Response> {
    let response: Response;
    try {
      response = await fetch(`/api/v1/reports${name ? '/' + encodeURIComponent(name) : ''}`, {
        method,
        headers: this.headers(body === undefined ? {} : { 'Content-Type': 'application/json' }),
        body,
        cache: 'no-store',
      });
    } catch {
      throw new ReportError('The server cannot be reached.', 0);
    }
    if (!response.ok) {
      let message = `The server answered ${response.status}.`;
      try {
        message = ((await response.json()) as { error?: string }).error ?? message;
      } catch {
        // not a JSON error
      }
      if (response.status === 401) this.db.authRequired.set(true);
      throw new ReportError(message, response.status);
    }
    return response;
  }

  /** Finds out whether the server has reports; a server that wants a token still has them. */
  async probe(): Promise<boolean> {
    try {
      const response = await fetch('/api/v1/reports', { headers: this.headers(), cache: 'no-store' });
      this.available.set(response.status !== 404);
    } catch {
      this.available.set(null); // cannot tell: the next try may
    }
    return this.available() !== false;
  }

  async list(): Promise<ReportListEntry[]> {
    return (await (await this.request('GET', '')).json()) as ReportListEntry[];
  }

  async load(name: string): Promise<ReportDef> {
    return parseReport(await (await this.request('GET', name)).text());
  }

  /** Every report, skipping (and naming) the ones that cannot be read. */
  async loadAll(): Promise<{ reports: ReportDef[]; problems: string[] }> {
    const reports: ReportDef[] = [];
    const problems: string[] = [];
    await Promise.all(
      (await this.list()).map(async entry => {
        try {
          const r = await this.load(entry.name);
          reports.push({ ...r, name: entry.name });
        } catch (e) {
          problems.push(`${entry.name}: ${(e as Error).message}`);
        }
      }),
    );
    reports.sort((a, b) => a.name.localeCompare(b.name));
    return { reports, problems };
  }

  async save(report: ReportDef): Promise<void> {
    await this.request('PUT', report.name, JSON.stringify(report, null, 2));
    this.revision.update(n => n + 1);
  }

  async remove(name: string): Promise<void> {
    await this.request('DELETE', name);
    this.revision.update(n => n + 1);
  }

  // ---- fonts -----------------------------------------------------------------------------------------------------------

  /** The report font, measured for layout and (in the browser) registered for the preview. Fails when the server has none. */
  loadFonts(): Promise<Record<FontFace, TrueTypeFont>> {
    this.fonts ??= (async () => {
      const get = async (face: FontFace): Promise<{ data: ArrayBuffer; font: TrueTypeFont }> => {
        const response = await fetch(FONT_FILES[face]);
        if (!response.ok) throw new ReportError('This server has no report fonts: reports cannot be drawn. Build the tool with fonts/ or put them in the --public directory.', response.status);
        const data = await response.arrayBuffer();
        return { data, font: TrueTypeFont.parse(new Uint8Array(data)) };
      };
      const [regular, bold] = await Promise.all([get('regular'), get('bold')]);
      for (const [face, weight] of [[regular, '400'], [bold, '700']] as const) {
        const loaded = await new FontFace(FONT_FAMILY, face.data.slice(0), { weight }).load();
        document.fonts.add(loaded);
      }
      this.fontsReady.set(true);
      return { regular: regular.font, bold: bold.font };
    })().catch(e => {
      this.fonts = null; // try again next time
      throw e;
    });
    return this.fonts;
  }

  // ---- data ----------------------------------------------------------------------------------------------------------

  private async session(): Promise<SqlSocket> {
    if (this.socket?.connected) return this.socket;
    this.connecting ??= (async () => {
      const socket = new SqlSocket(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}${SOCKET_PATH}`, { token: this.db.token ?? undefined, readOnly: true });
      await socket.connect();
      this.socket = socket;
      return socket;
    })().finally(() => (this.connecting = null));
    return this.connecting;
  }

  /** Stops what the report session is running. */
  interrupt(): void {
    this.socket?.interrupt();
  }

  close(): void {
    this.socket?.close();
    this.socket = null;
  }

  /** One query on the read-only session, parameters bound by name. */
  async query(sql: string, params: ParamValues, maxRows = MAX_ROWS): Promise<{ data: DataSet; truncated: boolean }> {
    const bound = bindNames(sql);
    const values = bound.names.map(n => toParam(params[n] ?? null));
    let socket: SqlSocket;
    try {
      socket = await this.session();
      const { view } = await socket.exec(bound.sql, values, { maxRows });
      if (!view) return { data: { columns: [], rows: [] }, truncated: false };
      const rows: Value[][] = [];
      for (let r = 0; r < view.rowCount; r++) {
        const row: Value[] = [];
        for (let c = 0; c < view.columns.length; c++) row.push(plain(view.cell(r, c)));
        rows.push(row);
      }
      return { data: { columns: [...view.columns], rows }, truncated: view.truncated };
    } catch (e) {
      if (e instanceof SocketError) {
        if (e.code === ErrorCode.PermissionDenied) this.db.authRequired.set(true);
        throw new ReportError(e.message, 400);
      }
      throw e;
    }
  }

  /** The choices of a parameter: its fixed list, or the rows of its query. */
  async options(p: ParamDef, params: ParamValues): Promise<ParamOption[]> {
    if (p.options?.length) return p.options;
    if (!p.optionsSql) return [];
    const { data } = await this.query(p.optionsSql, params, 1000);
    return data.rows.map(r => ({ value: (r[0] ?? '') as string | number, label: r.length > 1 ? String(r[1] ?? '') : undefined }));
  }

  /** Runs every dataset of the report. A failing dataset stops the run and says which. */
  private async datasets(report: ReportDef, params: ParamValues): Promise<{ datasets: Record<string, DataSet>; warnings: string[] }> {
    const datasets: Record<string, DataSet> = {};
    const warnings: string[] = [];
    for (const d of report.datasets) {
      try {
        const { data, truncated } = await this.query(d.sql, params);
        datasets[d.name] = data;
        if (truncated) warnings.push(`dataset ${d.name} was cut at ${MAX_ROWS.toLocaleString()} rows`);
      } catch (e) {
        throw new ReportError(`dataset ${d.name}: ${(e as Error).message}`, (e as ReportError).status ?? 400);
      }
    }
    return { datasets, warnings };
  }

  /** Runs the report: its datasets, and the datasets of the sub-reports it holds. */
  async run(report: ReportDef, params: ParamValues): Promise<RunResult> {
    const { datasets, warnings } = await this.datasets(report, params);
    const { subs, warnings: more } = await resolveSubreports(report, params, datasets, {
      load: async name => {
        try {
          return await this.load(name);
        } catch {
          return null;
        }
      },
      run: (target, p) => this.datasets(target, p),
    });
    return { datasets, subs, warnings: [...warnings, ...more] };
  }

  // ---- output ----------------------------------------------------------------------------------------------------------

  async layout(report: ReportDef, params: ParamValues, datasets: Record<string, DataSet>, view?: ViewState, subs?: SubRuns): Promise<LayoutResult> {
    return layoutReport({ report, params, datasets, fonts: await this.loadFonts(), view, subs });
  }

  async pdf(result: LayoutResult, report: ReportDef): Promise<Blob> {
    const bytes = await buildPdf(result.pages, await this.loadFonts(), { title: report.title || report.name, creator: 'libsql sql tool' }, { rasterize });
    return new Blob([bytes as BlobPart], { type: 'application/pdf' });
  }

  /** An Excel workbook of the run: a sheet per table, cross-tab and chart, with real numbers. */
  async xlsx(report: ReportDef, params: ParamValues, run: RunResult, view?: ViewState): Promise<Blob> {
    const { blocks } = buildBlocks({ report, params, datasets: run.datasets, subs: run.subs, view });
    const bytes = await buildXlsx({ report, params, blocks });
    return new Blob([bytes as BlobPart], { type: 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet' });
  }

  /** A Word document of the run: text, tables, cross-tabs, pictures, and the charts as pictures. */
  async docx(report: ReportDef, params: ParamValues, run: RunResult, view?: ViewState): Promise<Blob> {
    const fonts = await this.loadFonts();
    const m = { width: (t: string, f: FontFace, s: number): number => fonts[f].measure(t, s) };
    const { blocks } = buildBlocks({ report, params, datasets: run.datasets, subs: run.subs, view });
    const bytes = await buildDocx(
      { report, params, blocks },
      {
        rasterize: rasterizePng,
        chart: async (b, width) => {
          const it = b.item;
          const spec: ChartSpec = { kind: it.chart, stacked: it.stacked, horizontal: it.horizontal, donut: it.donut, labels: it.labels };
          const prims = drawChart(spec, b.data, { x: 0, y: 0, w: width, h: it.height }, { title: it.title, legend: it.legend, format: it.format }, m, { item: 0, hidden: b.hidden });
          return { png: await primsToPng(prims, width, it.height, FONT_FAMILY, 2), width, height: it.height };
        },
      },
    );
    return new Blob([bytes as BlobPart], { type: 'application/vnd.openxmlformats-officedocument.wordprocessingml.document' });
  }

  /** The rows of a table (or any result) as a PDF, an Excel workbook or a Word document. */
  async exportRows(format: ExportFormat, source: RowsExport): Promise<Blob> {
    const report = rowsReport(source.title, source.columns);
    const run: RunResult = { datasets: { rows: { columns: source.columns, rows: source.rows.map(row => row.map(plain)) } }, subs: {}, warnings: [] };
    switch (format) {
      case 'pdf':
        return this.pdf(await this.layout(report, {}, run.datasets, undefined, run.subs), report);
      case 'xlsx':
        return this.xlsx(report, {}, run);
      case 'docx':
        return this.docx(report, {}, run);
    }
  }
}

/** Rows of a result as one table of a throwaway report, so that they leave as the same PDF, Excel and Word files a report does. */
export interface RowsExport {
  title: string;
  columns: string[];
  rows: unknown[][];
}

export type ExportFormat = 'pdf' | 'xlsx' | 'docx';

/** The report behind {@link ReportService.exportRows}: a heading and one table of every column, landscape once it is wide. */
export function rowsReport(title: string, columns: string[]): ReportDef {
  const base = newReport(title);
  return {
    ...base,
    page: { ...base.page, orientation: columns.length > 5 ? 'landscape' : 'portrait' },
    datasets: [{ name: 'rows', sql: 'SELECT 1' }],
    body: [
      { type: 'text', text: '{{ReportName}}', style: { face: 'bold', size: 16 } },
      { type: 'spacer', height: 6 },
      { type: 'table', dataset: 'rows', zebra: true, columns: columns.map(field => ({ field, header: field })) },
    ],
  };
}

/** A cell of a result as a plain value. */
function plain(cell: unknown): Value {
  if (cell === null || typeof cell === 'number' || typeof cell === 'string') return cell;
  const c = cell as { type: string; value: string; size?: number };
  if (c.type === 'integer') return Number(c.value); // beyond 2^53 this loses digits, which reports can live with
  if (c.type === 'blob') return `[blob ${c.size ?? 0} bytes]`;
  return String(cell);
}

function toParam(v: Value) {
  if (v === null) return null;
  if (typeof v === 'boolean') return { type: ParamType.Integer, value: v ? 1 : 0 } as const;
  if (typeof v === 'number') return Number.isInteger(v) ? ({ type: ParamType.Integer, value: v } as const) : ({ type: ParamType.Real, value: v } as const);
  return { type: ParamType.Text, value: v } as const;
}
