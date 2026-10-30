// Copyright (c) 2026 Philipp Orlov

import { Component, input, output } from '@angular/core';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';

/** A text box that is Angular Material's: an outlined, compact `matInput` (or a textarea, with `rows`) with a label. */
@Component({
  selector: 'app-text',
  imports: [MatFormFieldModule, MatInputModule],
  template: `
    <mat-form-field appearance="outline" subscriptSizing="dynamic" class="field" [style.width.px]="type() === 'number' ? 104 : null">
      @if (label()) {
        <mat-label>{{ label() }}</mat-label>
      }
      @if (rows() > 0) {
        <textarea matInput [class.mono]="mono()" [rows]="rows()" [value]="value() ?? ''" [placeholder]="placeholder()" [attr.aria-label]="label() || ariaLabel() || null" spellcheck="false" (change)="onChange($event)" (input)="live() && onChange($event)"></textarea>
      } @else {
        <input matInput [class.mono]="mono()" [type]="type()" [attr.min]="min()" [attr.max]="max()" [attr.step]="step()" [value]="value() ?? ''" [placeholder]="placeholder()" [attr.aria-label]="label() || ariaLabel() || null" (change)="onChange($event)" (input)="live() && onChange($event)" />
      }
    </mat-form-field>
  `,
  styleUrl: './field.scss',
})
export class TextComponent {
  readonly label = input('');
  readonly ariaLabel = input('');
  readonly value = input<string | number | null | undefined>('');
  readonly type = input<'text' | 'number'>('text');
  readonly placeholder = input('');
  readonly min = input<number | null>(null);
  readonly max = input<number | null>(null);
  readonly step = input<number | null>(null);
  /** More than 0 makes it a multi-line box of that many rows. */
  readonly rows = input(0);
  readonly mono = input(false);
  /** Emit on every keystroke rather than when the box is left. */
  readonly live = input(false);
  readonly valueChange = output<string>();

  protected onChange(event: Event): void {
    this.valueChange.emit((event.target as HTMLInputElement | HTMLTextAreaElement).value);
  }
}
