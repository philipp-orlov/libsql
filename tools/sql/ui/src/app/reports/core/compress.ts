// Copyright (c) 2026 Philipp Orlov

// Deflate, inflate and CRC-32 on the platform's own streams (browsers and Node 18+), for the PDF, PNG and zip code.
// Free of Angular imports on purpose.

async function through(data: Uint8Array, stream: { writable: WritableStream<BufferSource>; readable: ReadableStream<Uint8Array> }): Promise<Uint8Array> {
  const writer = stream.writable.getWriter();
  void writer.write(data as BufferSource).catch(() => undefined);
  void writer.close().catch(() => undefined);
  return new Uint8Array(await new Response(stream.readable).arrayBuffer());
}

export const canCompress = (): boolean => typeof CompressionStream !== 'undefined';

/** zlib format (PDF's FlateDecode, PNG's IDAT), or null where there is no CompressionStream. */
export async function deflate(data: Uint8Array): Promise<Uint8Array | null> {
  return canCompress() ? through(data, new CompressionStream('deflate')) : null;
}

/** Raw deflate (what a zip entry holds), or null where there is no CompressionStream. */
export async function deflateRaw(data: Uint8Array): Promise<Uint8Array | null> {
  return canCompress() ? through(data, new CompressionStream('deflate-raw')) : null;
}

/** The bytes inside a zlib stream. */
export async function inflate(data: Uint8Array): Promise<Uint8Array> {
  return through(data, new DecompressionStream('deflate'));
}

/** The bytes of a raw deflate stream (a zip entry). */
export async function inflateRaw(data: Uint8Array): Promise<Uint8Array> {
  return through(data, new DecompressionStream('deflate-raw'));
}

let table: Uint32Array | null = null;
export function crc32(data: Uint8Array): number {
  if (!table) {
    table = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      table[n] = c >>> 0;
    }
  }
  let c = 0xffffffff;
  for (let i = 0; i < data.length; i++) c = table[(c ^ data[i]) & 255] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
