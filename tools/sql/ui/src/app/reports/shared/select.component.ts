// Copyright (c) 2026 Philipp Orlov

import { Component, computed, input, output } from '@angular/core';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatSelectModule } from '@angular/material/select';

export interface Choice {
  value: string;
  label: string;
}

/** A dropdown that is Angular Material's: an outlined, compact `mat-select` with a label, for the report editors and forms. */
@Component({
  selector: 'app-select',
  imports: [MatFormFieldModule, MatSelectModule],
  template: `
    <mat-form-field appearance="outline" subscriptSizing="dynamic" class="field" [style.width.px]="width()">
      @if (label()) {
        <mat-label>{{ label() }}</mat-label>
      }
      <mat-select [value]="value()" (selectionChange)="valueChange.emit($event.value)" [aria-label]="label() || 'Choose'" panelClass="report-panel" [disabled]="disabled()">
        @for (o of choices(); track o.value) {
          <mat-option [value]="o.value">{{ o.label }}</mat-option>
        }
      </mat-select>
    </mat-form-field>
  `,
  styleUrl: './field.scss',
})
export class SelectComponent {
  readonly label = input('');
  readonly value = input('');
  /** Plain strings stand for themselves; `{ value, label }` pairs show the label. */
  readonly options = input<(string | Choice)[]>([]);
  readonly width = input(150);
  readonly disabled = input(false);
  readonly valueChange = output<string>();

  protected readonly choices = computed<Choice[]>(() => this.options().map(o => (typeof o === 'string' ? { value: o, label: o } : o)));
}
