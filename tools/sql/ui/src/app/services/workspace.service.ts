import { Injectable, signal } from '@angular/core';

export const BROWSE_TAB = 1;

/** Which tab is open and which table the Browse Data tab shows. */
@Injectable({ providedIn: 'root' })
export class WorkspaceService {
  readonly tab = signal(0);
  readonly table = signal<string | null>(null);

  openTable(name: string): void {
    this.table.set(name);
    this.tab.set(BROWSE_TAB);
  }
}
