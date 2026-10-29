// Copyright (c) 2026 Philipp Orlov

// The report fonts for specs that run in Node (scripts/unit.mjs sets REPORT_FONTS to tools/sql/fonts).
import * as fs from 'node:fs';
import * as path from 'node:path';
import { FontFace } from './primitives';
import { TrueTypeFont } from './ttf';

export function testFonts(): Record<FontFace, TrueTypeFont> {
  const dir = process.env['REPORT_FONTS'] ?? '';
  const load = (f: string): TrueTypeFont => TrueTypeFont.parse(new Uint8Array(fs.readFileSync(path.join(dir, f))));
  return { regular: load('LiberationSans-Regular.ttf'), bold: load('LiberationSans-Bold.ttf') };
}
