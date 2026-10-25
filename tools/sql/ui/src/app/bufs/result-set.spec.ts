import { GOLDEN } from './golden';
import { BufsError, ResultSetView, formatDatetime } from './index';

function decode(base64: string): ArrayBuffer {
  const text = atob(base64);
  const bytes = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) {
    bytes[i] = text.charCodeAt(i);
  }
  return bytes.buffer;
}

describe('ResultSetView against the C++ writer', () => {
  for (const golden of GOLDEN) {
    it(`reads ${golden.name} cell for cell`, () => {
      const view = ResultSetView.open(decode(golden.bufs));
      expect(view.columns).toEqual(golden.columns);
      expect(view.rowCount).toBe(golden.rows.length);
      for (let r = 0; r < golden.rows.length; r++) {
        for (let c = 0; c < golden.columns.length; c++) {
          expect(view.cell(r, c)).withContext(`${golden.name} row ${r} column ${c}`).toEqual(golden.rows[r][c] as never);
        }
      }
    });
  }

  it('reads a stream that does not start 8-aligned inside its buffer', () => {
    const golden = GOLDEN.find((g) => g.name === 'batches')!;
    const original = new Uint8Array(decode(golden.bufs));
    const shifted = new Uint8Array(original.length + 3);
    shifted.set(original, 3);
    const view = ResultSetView.open(shifted.buffer, 3, original.length);
    expect(view.batchCount).toBeGreaterThan(1);
    expect(view.rowCount).toBe(golden.rows.length);
    expect(view.cell(golden.rows.length - 1, 0)).toEqual(golden.rows[golden.rows.length - 1][0] as never);
  });

  it('keeps big integers exact through bigint()', () => {
    const golden = GOLDEN.find((g) => g.name === 'int_i64_extremes')!;
    const column = ResultSetView.open(decode(golden.bufs)).batch(0).column(0);
    expect(column.bigint(0)).toBe(-9223372036854775808n);
    const hi = ResultSetView.open(decode(golden.bufs)).batch(0).column(1);
    expect(hi.bigint(0)).toBe(9223372036854775807n);
  });

  it('knows the full size of a cut blob', () => {
    const golden = GOLDEN.find((g) => g.name === 'blob_prefix')!;
    const column = ResultSetView.open(decode(golden.bufs)).batch(0).column(0);
    expect(column.blobSize(1)).toBe(46);
    expect(column.raw(1).length).toBe(32);
  });

  it('refuses what is not a result', () => {
    expect(() => ResultSetView.open(new ArrayBuffer(0))).toThrowError(BufsError);
    expect(() => ResultSetView.open(new Uint8Array(16).buffer)).toThrowError(BufsError);
    const golden = GOLDEN[0];
    const bytes = new Uint8Array(decode(golden.bufs));
    expect(() => ResultSetView.open(bytes.slice(0, bytes.length - 8).buffer)).toThrowError(BufsError);
  });
});

describe('formatDatetime', () => {
  it('matches sql::formatDatetime', () => {
    expect(formatDatetime(1709296496000000n)).toBe('2024-03-01 12:34:56');
    expect(formatDatetime(1709296496000250n)).toBe('2024-03-01 12:34:56.000250');
    expect(formatDatetime(-14182940000000n)).toBe('1969-07-20 20:17:40');
    expect(formatDatetime(0n)).toBe('1970-01-01 00:00:00');
  });
});
