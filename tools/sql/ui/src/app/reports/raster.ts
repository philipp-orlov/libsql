// Copyright (c) 2026 Philipp Orlov

// Drawing primitives and pictures onto a canvas, for the places that need pixels: a chart inside a Word document, an SVG
// or WebP picture inside a PDF or Word file. Browser only (the core has no DOM).

import { Prim, Raster } from './core';

type Canvas2D = CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D;

function canvas(width: number, height: number): { canvas: HTMLCanvasElement | OffscreenCanvas; ctx: Canvas2D } {
  const c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(width, height) : Object.assign(document.createElement('canvas'), { width, height });
  const ctx = c.getContext('2d') as Canvas2D | null;
  if (!ctx) throw new Error('no canvas');
  return { canvas: c, ctx };
}

async function png(c: HTMLCanvasElement | OffscreenCanvas): Promise<Uint8Array> {
  const blob = c instanceof HTMLCanvasElement ? await new Promise<Blob | null>(r => c.toBlob(r, 'image/png')) : await c.convertToBlob({ type: 'image/png' });
  if (!blob) throw new Error('the canvas gave no picture');
  return new Uint8Array(await blob.arrayBuffer());
}

/** Draws the primitives (not pictures) with the font family the preview uses. */
export function drawPrims(ctx: Canvas2D, prims: Prim[], family: string): void {
  for (const p of prims) {
    if ('hit' in p && p.hit?.ghost) continue;
    ctx.save();
    switch (p.k) {
      case 'text':
        ctx.font = `${p.face === 'bold' ? 700 : 400} ${p.size}px ${family}`;
        ctx.fillStyle = p.color;
        ctx.fillText(p.text, p.x, p.y);
        break;
      case 'rect': {
        ctx.beginPath();
        if (p.r) ctx.roundRect(p.x, p.y, p.w, p.h, p.r);
        else ctx.rect(p.x, p.y, p.w, p.h);
        if (p.fill) {
          ctx.globalAlpha = p.fillOpacity ?? 1;
          ctx.fillStyle = p.fill;
          ctx.fill();
          ctx.globalAlpha = 1;
        }
        if (p.stroke) {
          ctx.strokeStyle = p.stroke;
          ctx.lineWidth = p.lw ?? 1;
          ctx.stroke();
        }
        break;
      }
      case 'line':
        ctx.beginPath();
        ctx.setLineDash(p.dash ?? []);
        ctx.strokeStyle = p.color;
        ctx.lineWidth = p.lw;
        ctx.moveTo(p.x1, p.y1);
        ctx.lineTo(p.x2, p.y2);
        ctx.stroke();
        break;
      case 'path':
        ctx.beginPath();
        for (const c of p.cmds) {
          if (c[0] === 'M') ctx.moveTo(c[1], c[2]);
          else if (c[0] === 'L') ctx.lineTo(c[1], c[2]);
          else if (c[0] === 'C') ctx.bezierCurveTo(c[1], c[2], c[3], c[4], c[5], c[6]);
          else ctx.closePath();
        }
        if (p.fill) {
          ctx.globalAlpha = p.fillOpacity ?? 1;
          ctx.fillStyle = p.fill;
          ctx.fill();
          ctx.globalAlpha = 1;
        }
        if (p.stroke) {
          ctx.strokeStyle = p.stroke;
          ctx.lineWidth = p.lw ?? 1;
          ctx.lineJoin = 'round';
          ctx.stroke();
        }
        break;
      case 'image':
        break;
    }
    ctx.restore();
  }
}

/** The primitives of a `width` x `height` point box as a PNG, `scale` pixels to the point. */
export async function primsToPng(prims: Prim[], width: number, height: number, family: string, scale = 2): Promise<Uint8Array> {
  const { canvas: c, ctx } = canvas(Math.ceil(width * scale), Math.ceil(height * scale));
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0, 0, c.width, c.height);
  ctx.scale(scale, scale);
  drawPrims(ctx, prims, family);
  return png(c);
}

async function load(src: string): Promise<HTMLImageElement> {
  const img = new Image();
  img.decoding = 'async';
  img.src = src;
  await img.decode();
  return img;
}

/** A picture (SVG, WebP, GIF ...) as pixels at `width` x `height`. */
export async function rasterize(src: string, width: number, height: number): Promise<Raster | null> {
  try {
    const img = await load(src);
    const { ctx } = canvas(width, height);
    ctx.drawImage(img, 0, 0, width, height);
    const data = ctx.getImageData(0, 0, width, height).data;
    const rgb = new Uint8Array(width * height * 3);
    const alpha = new Uint8Array(width * height);
    let translucent = false;
    for (let i = 0, j = 0, k = 0; i < data.length; i += 4, j += 3, k++) {
      rgb[j] = data[i];
      rgb[j + 1] = data[i + 1];
      rgb[j + 2] = data[i + 2];
      alpha[k] = data[i + 3];
      if (data[i + 3] !== 255) translucent = true;
    }
    return { width, height, rgb, alpha: translucent ? alpha : undefined };
  } catch {
    return null;
  }
}

/** A picture as a PNG at `width` x `height`. */
export async function rasterizePng(src: string, width: number, height: number): Promise<Uint8Array | null> {
  try {
    const img = await load(src);
    const { canvas: c, ctx } = canvas(width, height);
    ctx.drawImage(img, 0, 0, width, height);
    return await png(c);
  } catch {
    return null;
  }
}
