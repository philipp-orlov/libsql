import { Directive, TemplateRef, inject } from '@angular/core';

import { GridColumn, GridGroupRow } from './data-grid.models';

export interface GridCellContext<T = unknown> {
  /** The column's value. */
  $implicit: unknown;
  row: T;
  /** Index of the row in the grid's data. */
  index: number;
  column: GridColumn<T>;
  /** The value as the column shows it as text. */
  text: string;
}

export interface GridGroupHeaderContext {
  $implicit: GridGroupRow;
}

/** Renders the data cells: `<ng-template gridCell let-value let-row="row" let-column="column">`. */
@Directive({ selector: 'ng-template[gridCell]' })
export class GridCellDirective<T = unknown> {
  readonly template = inject<TemplateRef<GridCellContext<T>>>(TemplateRef);

  static ngTemplateContextGuard<T>(_directive: GridCellDirective<T>, context: unknown): context is GridCellContext<T> {
    return true;
  }
}

/** Renders what follows the toggle of a group header: `<ng-template gridGroupHeader let-group>`. */
@Directive({ selector: 'ng-template[gridGroupHeader]' })
export class GridGroupHeaderDirective {
  readonly template = inject<TemplateRef<GridGroupHeaderContext>>(TemplateRef);

  static ngTemplateContextGuard(_directive: GridGroupHeaderDirective, context: unknown): context is GridGroupHeaderContext {
    return true;
  }
}
