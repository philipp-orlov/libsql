// Copyright (c) 2026 Philipp Orlov

import { Component, computed, input, output } from '@angular/core';
import { MatDatepickerModule } from '@angular/material/datepicker';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { provideNativeDateAdapter } from '@angular/material/core';

/** A date parameter with Material's date picker. The value is `yyyy-MM-dd` (what the queries bind), or '' for none. */
@Component({
  selector: 'app-date',
  imports: [MatDatepickerModule, MatFormFieldModule, MatInputModule],
  providers: [provideNativeDateAdapter()],
  template: `
    <mat-form-field appearance="outline" subscriptSizing="dynamic" class="field" [style.width.px]="width()">
      @if (label()) {
        <mat-label>{{ label() }}</mat-label>
      }
      <input matInput [matDatepicker]="picker" [value]="date()" (dateChange)="valueChange.emit(format($event.value))" [attr.aria-label]="label() || 'Date'" />
      <mat-datepicker-toggle matIconSuffix [for]="picker" />
      <mat-datepicker #picker panelClass="report-panel" />
    </mat-form-field>
  `,
  styleUrl: './field.scss',
})
export class DateComponent {
  readonly label = input('');
  readonly value = input('');
  readonly width = input(170);
  readonly valueChange = output<string>();

  /** In the local time zone, so that the day shown is the day typed. */
  protected readonly date = computed(() => {
    const m = /^(\d{4})-(\d{2})-(\d{2})/.exec(this.value());
    return m ? new Date(Number(m[1]), Number(m[2]) - 1, Number(m[3])) : null;
  });

  protected format(d: Date | null): string {
    return d ? `${String(d.getFullYear()).padStart(4, '0')}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}` : '';
  }
}
