// Copyright (c) 2026 Philipp Orlov

import { checkTemplate, evaluate, ExprContext, ExprError, interpolate } from './expr';
import { Value } from './format';

const columns = ['region', 'amount'];
const rows: Value[][] = [['N', 10], ['S', 30], ['N', null]];
function cx(row: Value[] | null = rows[0], scope: Value[][] = rows): ExprContext {
  return {
    rows: scope,
    row,
    lookup(name, r) {
      const i = columns.indexOf(name);
      if (r && i >= 0) return r[i];
      if (name === 'year') return 2026;
      if (name === 'PageNumber') return 3;
      return undefined;
    },
  };
}

describe('expressions', () => {
  it('does arithmetic with the usual precedence', () => {
    expect(evaluate('1 + 2 * 3', cx())).toBe(7);
    expect(evaluate('(1 + 2) * 3', cx())).toBe(9);
    expect(evaluate('10 % 4 - -1', cx())).toBe(3);
    expect(evaluate('7 / 2', cx())).toBe(3.5);
    expect(evaluate('1 / 0', cx())).toBeNull();
  });
  it('compares, combines and chooses', () => {
    expect(evaluate('amount > 5 && region == "N"', cx())).toBe(true);
    expect(evaluate('amount > 50 || !(1 == 2)', cx())).toBe(true);
    expect(evaluate('amount == 10 ? "ten" : "other"', cx())).toBe('ten');
    expect(evaluate("'a' < 'b'", cx())).toBe(true);
  });
  it('joins text with + and reads names, parameters and brackets', () => {
    expect(evaluate('region + "-" + year', cx())).toBe('N-2026');
    expect(evaluate('[amount] * 2', cx())).toBe(20);
  });
  it('aggregates over the scope', () => {
    expect(evaluate('Sum(amount)', cx())).toBe(40);
    expect(evaluate('Avg(amount)', cx())).toBe(20);
    expect(evaluate('Min(amount)', cx())).toBe(10);
    expect(evaluate('Max(amount)', cx())).toBe(30);
    expect(evaluate('Count()', cx())).toBe(3);
    expect(evaluate('Count(amount)', cx())).toBe(2);
    expect(evaluate('First(region)', cx())).toBe('N');
    expect(evaluate('Last(region)', cx())).toBe('N');
    expect(evaluate('Sum(amount * 2)', cx())).toBe(80);
  });
  it('has the helper functions', () => {
    expect(evaluate('Format(1234.5, "#,##0.0")', cx())).toBe('1,234.5');
    expect(evaluate('Upper("ab") + Lower("CD") + Trim("  e ")', cx())).toBe('ABcde');
    expect(evaluate('Len("héllo")', cx())).toBe(5);
    expect(evaluate('Round(2.456, 2)', cx())).toBe(2.46);
    expect(evaluate('Abs(-3)', cx())).toBe(3);
    expect(evaluate('Iif(1 > 2, "a", "b")', cx())).toBe('b');
    expect(evaluate('Coalesce(null, "", "x")', cx())).toBe('x');
    expect(evaluate('Substring("abcdef", 1, 3)', cx())).toBe('bcd');
  });
  it('rejects what it does not understand', () => {
    expect(() => evaluate('1 +', cx())).toThrowError(ExprError);
    expect(() => evaluate('nothing', cx())).toThrowError(/unknown name/);
    expect(() => evaluate('Nope(1)', cx())).toThrowError(/unknown function/);
    expect(() => evaluate('"open', cx())).toThrowError(/unterminated/);
    expect(() => evaluate('1 ? 2', cx())).toThrowError(/':'/);
    expect(() => evaluate('a $ b', cx())).toThrowError(ExprError);
  });
  it('cannot run code', () => {
    expect(() => evaluate('constructor.constructor("return 1")()', cx())).toThrowError(ExprError);
    expect(() => evaluate('process', cx())).toThrowError(/unknown name/);
  });
});

describe('templates', () => {
  it('replaces every {{ }} and shows #Error for a bad one', () => {
    expect(interpolate('Page {{PageNumber}} of {{ 1 + 1 }} {{ bogus }}!', cx())).toBe('Page 3 of 2 #Error!');
    expect(interpolate('no expressions', cx())).toBe('no expressions');
    expect(interpolate('{{ Sum(amount) }} in {{ Count() }} rows', cx(null))).toBe('40 in 3 rows');
  });
  it('checks syntax without needing names', () => {
    expect(checkTemplate('ok {{ a + 1 }}')).toBeNull();
    expect(checkTemplate('bad {{ 1 + }}')).toMatch(/unexpected/);
  });
});
