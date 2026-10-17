import { Cell } from '../models/api.models';
import { cellEditText, cellKind, cellText, inputToCell, isEditableCell, toCsv } from './cell';

describe('cell', () => {
  const blob: Cell = { type: 'blob', size: 40, value: '00ff' };
  const big: Cell = { type: 'integer', value: '9007199254740993' };

  it('tells the kinds of cell apart', () => {
    expect(cellKind(null)).toBe('null');
    expect(cellKind(7)).toBe('integer');
    expect(cellKind(1.5)).toBe('real');
    expect(cellKind('x')).toBe('text');
    expect(cellKind(big)).toBe('integer');
    expect(cellKind(blob)).toBe('blob');
  });

  it('shows a cell as text', () => {
    expect(cellText(null)).toBe('NULL');
    expect(cellText(7)).toBe('7');
    expect(cellText('abc')).toBe('abc');
    expect(cellText(big)).toBe('9007199254740993');
    expect(cellText(blob)).toBe('BLOB (40 bytes)');
  });

  it('starts an edit from the value, not from the NULL or BLOB label', () => {
    expect(cellEditText(null)).toBe('');
    expect(cellEditText(blob)).toBe('');
    expect(cellEditText(12)).toBe('12');
    expect(cellEditText('a b')).toBe('a b');
  });

  it('does not offer to edit a blob', () => {
    expect(isEditableCell(blob)).toBeFalse();
    expect(isEditableCell(null)).toBeTrue();
    expect(isEditableCell('x')).toBeTrue();
  });

  describe('inputToCell', () => {
    const integer = { affinity: 'INTEGER' };
    const real = { affinity: 'REAL' };
    const text = { affinity: 'TEXT' };
    const none = { affinity: 'BLOB' };

    it('sends digits typed into an integer column as a number', () => {
      expect(inputToCell('42', integer)).toBe(42);
      expect(inputToCell(' -7 ', integer)).toBe(-7);
      expect(inputToCell('+5', integer)).toBe(5);
    });

    it('keeps an integer a JavaScript number cannot hold as an integer object', () => {
      expect(inputToCell('9007199254740993', integer)).toEqual({ type: 'integer', value: '9007199254740993' });
      expect(inputToCell('+12345678901234567', integer)).toEqual({ type: 'integer', value: '12345678901234567' });
    });

    it('reads decimals only for a column that can hold them', () => {
      expect(inputToCell('1.5', real)).toBe(1.5);
      expect(inputToCell('2e3', { affinity: 'NUMERIC' })).toBe(2000);
      expect(inputToCell('1.5', integer)).toBe('1.5');
    });

    it('leaves everything else as the text typed', () => {
      expect(inputToCell('42', text)).toBe('42');
      expect(inputToCell('42', none)).toBe('42');
      expect(inputToCell('abc', integer)).toBe('abc');
      expect(inputToCell('', integer)).toBe('');
      expect(inputToCell(' 12 ', text)).toBe(' 12 ');
    });
  });

  describe('toCsv', () => {
    it('writes a header and one line per row', () => {
      expect(toCsv(['a', 'b'], [[1, 'x'], [2, 'y']])).toBe('a,b\r\n1,x\r\n2,y\r\n');
    });

    it('writes NULL as an empty field and a blob as its label', () => {
      expect(toCsv(['a', 'b'], [[null, blob]])).toBe('a,b\r\n,BLOB (40 bytes)\r\n');
    });

    it('quotes a field with a comma, a quote or a line break', () => {
      expect(toCsv(['c'], [['a,b'], ['say "hi"'], ['two\nlines']])).toBe('c\r\n"a,b"\r\n"say ""hi"""\r\n"two\nlines"\r\n');
      expect(toCsv(['a,b'], [])).toBe('"a,b"\r\n');
    });
  });
});
