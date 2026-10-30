// Copyright (c) 2026 Philipp Orlov

// A laid-out page as SVG, for the on-screen preview. Text is drawn in the font family `family` (load the report's
// TrueType font under that name with the FontFace API) with kerning off, because the PDF has none: the lines break at
// the same places in both. Marks with interactivity (primitives.ts, Hit) carry it as data attributes that the viewer's
// event handlers read: data-k (highlight key), data-tip (JSON), data-act (JSON). Free of Angular imports on purpose.

import { Hit, PageDoc, Prim } from './primitives';

const esc = (s: string): string => s.replace(/[&<>"]/g, c => (c === '&' ? '&amp;' : c === '<' ? '&lt;' : c === '>' ? '&gt;' : '&quot;'));
const n = (v: number): string => String(Math.round(v * 100) / 100);

/** The attributes of a mark's interactivity; a ghost takes the pointer and draws nothing. */
function hitAttrs(hit: Hit | undefined, shape: { fill?: string; stroke?: string }): string {
  if (!hit) return ' pointer-events="none"';
  let out = hit.cls ? ` class="${esc(hit.cls)}"` : '';
  if (hit.key !== undefined) out += ` data-k="${esc(hit.key)}"`;
  if (hit.tip) out += ` data-tip="${esc(JSON.stringify(hit.tip))}"`;
  if (hit.action) out += ` data-act="${esc(JSON.stringify(hit.action))}"`;
  if (hit.fx) out += ` style="--dx:${n(hit.fx.dx)}px;--dy:${n(hit.fx.dy)}px"`;
  if (hit.ghost) out += ' pointer-events="all"';
  void shape;
  return out;
}

function prim(p: Prim, family: string): string {
  switch (p.k) {
    case 'text':
      return `<text x="${n(p.x)}" y="${n(p.y)}" font-family="${esc(family)}" font-size="${n(p.size)}" font-weight="${p.face === 'bold' ? 700 : 400}" fill="${p.color}" xml:space="preserve"${hitAttrs(p.hit, {})}>${esc(p.text)}</text>`;
    case 'rect': {
      const ghost = p.hit?.ghost;
      const fill = ghost ? 'transparent' : p.fill ?? 'none';
      const opacity = !ghost && p.fillOpacity !== undefined ? ` fill-opacity="${p.fillOpacity}"` : '';
      const stroke = !ghost && p.stroke ? ` stroke="${p.stroke}" stroke-width="${n(p.lw ?? 1)}"` : '';
      const round = p.r ? ` rx="${n(p.r)}"` : '';
      return `<rect x="${n(p.x)}" y="${n(p.y)}" width="${n(p.w)}" height="${n(p.h)}"${round} fill="${fill}"${opacity}${stroke}${hitAttrs(p.hit, p)}/>`;
    }
    case 'image':
      return `<image x="${n(p.x)}" y="${n(p.y)}" width="${n(p.w)}" height="${n(p.h)}" href="${esc(p.src)}" preserveAspectRatio="xMidYMid meet"${hitAttrs(p.hit, {})}/>`;
    case 'line':
      return `<line x1="${n(p.x1)}" y1="${n(p.y1)}" x2="${n(p.x2)}" y2="${n(p.y2)}" stroke="${p.color}" stroke-width="${n(p.lw)}"${p.dash ? ` stroke-dasharray="${p.dash.join(' ')}"` : ''} pointer-events="none"/>`;
    case 'path': {
      const d = p.cmds.map(c => (c[0] === 'Z' ? 'Z' : c[0] + c.slice(1).map(v => n(v as number)).join(' '))).join(' ');
      const opacity = p.fillOpacity !== undefined ? ` fill-opacity="${p.fillOpacity}"` : '';
      return `<path d="${d}" fill="${p.fill ?? 'none'}"${opacity}${p.stroke ? ` stroke="${p.stroke}" stroke-width="${n(p.lw ?? 1)}" stroke-linejoin="round"` : ''}${hitAttrs(p.hit, p)}/>`;
    }
  }
}

export function pageToSvg(page: PageDoc, family = 'ReportSans'): string {
  return (
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${n(page.width)} ${n(page.height)}" width="${n(page.width)}" height="${n(page.height)}" style="font-kerning:none">` +
    `<rect width="${n(page.width)}" height="${n(page.height)}" fill="#ffffff" pointer-events="none"/>` +
    page.prims.map(p => prim(p, family)).join('') +
    '</svg>'
  );
}
