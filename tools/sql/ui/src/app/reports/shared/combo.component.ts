// Copyright (c) 2026 Philipp Orlov

import { Component, computed, input, output, signal } from '@angular/core';
import { MatAutocompleteModule } from '@angular/material/autocomplete';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';

/**
 * A text field that offers what can be typed (Material's autocomplete): the datasets of a report, the fields of a dataset.
 * Any text is accepted, since a field may be one that has not been run yet; the value is committed on a choice, Enter or leaving it.
 */
@Component({
  selector: 'app-combo',
  imports: [MatAutocompleteModule, MatFormFieldModule, MatInputModule],
  template: `
    <mat-form-field appearance="outline" subscriptSizing="dynamic" class="field" [style.width.px]="width()">
      @if (label()) {
        <mat-label>{{ label() }}</mat-label>
      }
      <input matInput [class.mono]="mono()" [value]="typed() ?? value()" [placeholder]="placeholder()" [matAutocomplete]="auto" [attr.aria-label]="label() || placeholder()"
             (input)="typed.set($any($event.target).value)" (change)="commit($any($event.target).value)" (keydown.enter)="commit($any($event.target).value)" />
      <mat-autocomplete #auto="matAutocomplete" panelClass="report-panel" (optionSelected)="commit($event.option.value)">
        @for (s of shown(); track s) {
          <mat-option [value]="s">{{ s }}</mat-option>
        }
      </mat-autocomplete>
    </mat-form-field>
  `,
  styleUrl: './field.scss',
})
export class ComboComponent {
  readonly label = input('');
  readonly value = input('');
  readonly suggestions = input<string[]>([]);
  readonly placeholder = input('');
  readonly width = input(160);
  readonly mono = input(false);
  readonly valueChange = output<string>();

  /** What is being typed (null while the field shows the value). */
  protected readonly typed = signal<string | null>(null);
  protected readonly shown = computed(() => {
    const t = (this.typed() ?? '').toLowerCase();
    const all = this.suggestions();
    return t ? all.filter(s => s.toLowerCase().includes(t)) : all;
  });
  private last: string | null = null;

  protected commit(text: string): void {
    this.typed.set(null);
    const v = text.trim();
    // the choice and the change event both come: say it once
    if (v === this.last && v === this.value()) return;
    this.last = v;
    this.valueChange.emit(v);
  }
}
