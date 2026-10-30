#!/usr/bin/env node
// Post-build step: folds the main.js, styles.css, fonts and favicon.ico that `npm run b` emits into
// dist/app back into one dependency-free index.html. It is written to dist/app/single/ and to
// ../public/index.html, which the sql tool build links into the binary (cmake/EmbedIndexHtml.cmake).
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const browserDir = join(process.cwd(), 'dist/app');
const outDir = join(browserDir, 'single');
const toolFile = fileURLToPath(new URL('../../public/index.html', import.meta.url));

let html = readFileSync(join(browserDir, 'index.html'), 'utf8');

const MIME_TYPES = { '.woff2': 'font/woff2', '.woff': 'font/woff', '.ico': 'image/x-icon' };

const dataUri = (href) => {
  const type = MIME_TYPES[href.slice(href.lastIndexOf('.'))];
  if (!type) throw new Error(`no MIME type for ${href}`);
  return `data:${type};base64,${readFileSync(join(browserDir, href)).toString('base64')}`;
};

// The <noscript> fallback repeats the stylesheet link; the CSS below is inlined unconditionally.
html = html.replace(/<noscript>.*?<\/noscript>/gs, '');

// Fonts the stylesheet refers to become data URIs, so that the CSS needs no second file.
const inlineFonts = (css) => css.replace(/url\("?(\.\/media\/[^")]+)"?\)/g, (match, href) => `url("${dataUri(href)}")`);

html = html.replace(/<link rel="stylesheet" href="([^"]+)"[^>]*>/g, (match, href) => {
  if (/^https?:/.test(href)) return match;
  return `<style>${inlineFonts(readFileSync(join(browserDir, href), 'utf8'))}</style>`;
});

// A script may not contain "</script"; in JavaScript "<\/script" means the same text.
html = html.replace(/<script((?:(?!src=)[^>])*)\ssrc="([^"]+)"((?:(?!src=)[^>])*)><\/script>/g, (match, before, src, after) => {
  if (/^https?:/.test(src)) return match;
  return `<script${before}${after}>${readFileSync(join(browserDir, src), 'utf8').replace(/<\/script/gi, '<\\/script')}</script>`;
});

html = html.replace(/href="favicon\.ico"/, () => `href="${dataUri('favicon.ico')}"`);

mkdirSync(outDir, { recursive: true });
writeFileSync(join(outDir, 'index.html'), html);
console.log(`Single-file bundle written to ${join(outDir, 'index.html')} (${(Buffer.byteLength(html) / 1024).toFixed(1)} KiB)`);
mkdirSync(join(toolFile, '..'), { recursive: true });
writeFileSync(toolFile, html);
console.log(`Copied to ${toolFile}, the web UI the sql tool build embeds`);
