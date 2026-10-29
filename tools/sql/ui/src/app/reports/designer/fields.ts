// Copyright (c) 2026 Philipp Orlov

import { DataSet } from '../core';

/** What a field is for: text and dates slice the data (dimensions), numbers are summed up (measures). */
export type FieldKind = 'text' | 'number' | 'date';

export interface FieldInfo {
  name: string;
  kind: FieldKind;
}

/** What is dragged from the data pane: a column of a dataset. */
export interface FieldDrag {
  dataset: string;
  field: string;
  kind: FieldKind;
}

export const FIELD_MIME = 'application/x-report-field';

const DATE = /^\d{4}-\d{2}(-\d{2})?([ T]\d{2}:\d{2}.*)?$/;

/** The kind of each column, from the first value that is not NULL. */
export function fieldsOf(data: DataSet): FieldInfo[] {
  return data.columns.map((name, i) => {
    const v = data.rows.find(r => r[i] !== null && r[i] !== undefined)?.[i];
    return { name, kind: typeof v === 'number' ? 'number' : typeof v === 'string' && DATE.test(v) ? 'date' : 'text' };
  });
}

export const KIND_ICON: Record<FieldKind, string> = { text: 'abc', number: 'tag', date: 'calendar_today' };
export const KIND_LABEL: Record<FieldKind, string> = { text: 'Text (a dimension)', number: 'Number (a measure)', date: 'Date' };
