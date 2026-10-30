// Copyright (c) 2026 Philipp Orlov

import { Component, input, signal } from '@angular/core';
import { MatIconModule } from '@angular/material/icon';

import { ParamDef } from '../core';
import { FIELD_MIME, FieldDrag, FieldInfo, KIND_ICON, KIND_LABEL } from './fields';

/** The datasets of the report with their fields, and its parameters: what you build the report from. Drag a field out. */
@Component({
  selector: 'app-data-pane',
  imports: [MatIconModule],
  templateUrl: './data-pane.component.html',
  styleUrl: './data-pane.component.scss',
})
export class DataPaneComponent {
  readonly datasets = input<{ name: string; fields: FieldInfo[]; sampled: boolean }[]>([]);
  readonly parameters = input<ParamDef[]>([]);

  protected readonly collapsed = signal<Record<string, boolean>>({});
  protected readonly icon = KIND_ICON;
  protected readonly label = KIND_LABEL;

  protected toggle(name: string): void {
    this.collapsed.update(c => ({ ...c, [name]: !c[name] }));
  }

  protected drag(event: DragEvent, dataset: string, field: FieldInfo): void {
    const payload: FieldDrag = { dataset, field: field.name, kind: field.kind };
    event.dataTransfer?.setData(FIELD_MIME, JSON.stringify(payload));
    // a text box or an expression takes the plain text: a field is `{{name}}` there
    event.dataTransfer?.setData('text/plain', `{{[${field.name}]}}`);
    if (event.dataTransfer) event.dataTransfer.effectAllowed = 'copy';
  }

  protected dragParam(event: DragEvent, name: string): void {
    event.dataTransfer?.setData('text/plain', `{{${name}}}`);
    if (event.dataTransfer) event.dataTransfer.effectAllowed = 'copy';
  }
}
