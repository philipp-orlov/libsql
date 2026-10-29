// Copyright (c) 2026 Philipp Orlov

import { Component, input, output } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';

import { ParamDef, ParamType, ReportDef } from '../core';
import { assign, Edit, move, unique } from './editing';

const TYPES: ParamType[] = ['text', 'integer', 'real', 'date', 'boolean'];

/** The parameters a report asks for before it runs; datasets use them as :name. */
@Component({
  selector: 'app-parameters-editor',
  imports: [MatButtonModule, MatIconModule],
  templateUrl: './parameters-editor.component.html',
  styleUrl: './editor.scss',
})
export class ParametersEditorComponent {
  readonly report = input.required<ReportDef>();
  readonly edit = output<Edit>();
  protected readonly types = TYPES;

  protected add(): void {
    this.edit.emit(r => r.parameters.push({ name: unique('param', r.parameters.map(p => p.name)), type: 'text' }));
  }

  protected remove(i: number): void {
    this.edit.emit(r => void r.parameters.splice(i, 1));
  }

  protected shift(i: number, by: number): void {
    this.edit.emit(r => move(r.parameters, i, i + by));
  }

  protected set(i: number, path: string, value: unknown): void {
    this.edit.emit(r => assign(r.parameters[i], path, value));
  }

  /** `value=Label` per line. */
  protected optionsText(p: ParamDef): string {
    return (p.options ?? []).map(o => (o.label !== undefined ? `${o.value}=${o.label}` : String(o.value))).join('\n');
  }

  protected setOptions(i: number, text: string): void {
    this.edit.emit(r => {
      const p = r.parameters[i];
      const lines = text.split('\n').map(l => l.trim()).filter(Boolean);
      if (!lines.length) return void delete p.options;
      p.options = lines.map(line => {
        const eq = line.indexOf('=');
        const raw = eq < 0 ? line : line.slice(0, eq).trim();
        const value = p.type === 'integer' || p.type === 'real' ? (Number.isFinite(Number(raw)) ? Number(raw) : raw) : raw;
        return eq < 0 ? { value } : { value, label: line.slice(eq + 1).trim() };
      });
    });
  }

  protected setDefault(i: number, text: string, type: ParamType): void {
    const value: string | number | boolean | undefined =
      text.trim() === '' ? undefined : type === 'integer' || type === 'real' ? (Number.isFinite(Number(text)) ? Number(text) : text) : type === 'boolean' ? text === 'true' : text;
    this.edit.emit(r => assign(r.parameters[i], 'default', value));
  }

  protected defaultText(p: ParamDef): string {
    return p.default === undefined || p.default === null ? '' : String(p.default);
  }
}
