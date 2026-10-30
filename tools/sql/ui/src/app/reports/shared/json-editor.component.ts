// Copyright (c) 2026 Philipp Orlov

import { Component, computed, ElementRef, input, output, viewChild } from '@angular/core';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { jsonHtml } from '../core/jsonhtml';

/** A multi-line `matInput` for JSON that colors what is typed: the text is drawn by a `<pre>` behind a textarea whose own text is clear. */
@Component({
  selector: 'app-json-editor',
  imports: [MatFormFieldModule, MatInputModule],
  template: `
    <mat-form-field appearance="outline" subscriptSizing="dynamic" class="field">
      <div class="stack">
        <pre #back class="back" aria-hidden="true" [style.height.px]="rows() * 20" [innerHTML]="html()"></pre>
        <textarea #box matInput class="mono front" wrap="off" spellcheck="false" [rows]="rows()" [value]="value()" [attr.aria-label]="ariaLabel() || null" (input)="onInput($event)" (scroll)="sync()"></textarea>
      </div>
    </mat-form-field>
  `,
  styleUrl: './json-editor.component.scss',
})
export class JsonEditorComponent {
  readonly value = input('');
  readonly ariaLabel = input('');
  readonly rows = input(22);
  readonly valueChange = output<string>();

  private readonly back = viewChild.required<ElementRef<HTMLElement>>('back');
  private readonly box = viewChild.required<ElementRef<HTMLTextAreaElement>>('box');
  /** A trailing newline is given a line of its own so that both layers end at the same place. */
  protected readonly html = computed(() => jsonHtml(this.value()) + '\n');

  protected onInput(event: Event): void {
    this.valueChange.emit((event.target as HTMLTextAreaElement).value);
    this.sync();
  }

  protected sync(): void {
    const b = this.box().nativeElement;
    const p = this.back().nativeElement;
    p.scrollTop = b.scrollTop;
    p.scrollLeft = b.scrollLeft;
  }
}
