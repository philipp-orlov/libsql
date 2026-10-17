import { Component, computed, effect, inject, signal, untracked } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatCheckboxModule } from '@angular/material/checkbox';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatIconModule } from '@angular/material/icon';
import { MatInputModule } from '@angular/material/input';
import { MatSelectModule } from '@angular/material/select';

import { PragmaInfo, PragmaScope } from '../../models/api.models';
import { DatabaseService } from '../../services/database.service';
import { NoticeService } from '../../services/notice.service';
import { cellText } from '../../utils/cell';

const GROUPS: { scope: PragmaScope; title: string; note: string }[] = [
  { scope: 'file', title: 'Saved in the database file', note: 'Every program that opens the file sees these.' },
  {
    scope: 'session',
    title: 'This connection only',
    note: 'They belong to the connection of this tool. They are lost when the tool stops, and they do not change isvc or any other program that opens the file.',
  },
  { scope: 'store', title: 'Reported by the store', note: 'Read-only.' },
];

/** The Edit Pragmas tab. */
@Component({
  selector: 'app-pragmas',
  imports: [MatButtonModule, MatCheckboxModule, MatFormFieldModule, MatIconModule, MatInputModule, MatSelectModule],
  templateUrl: './pragmas.component.html',
  styleUrl: './pragmas.component.scss',
})
export class PragmasComponent {
  protected readonly db = inject(DatabaseService);
  private readonly notice = inject(NoticeService);

  protected readonly pragmas = signal<PragmaInfo[]>([]);
  /** Values the user changed and has not saved, by pragma name. */
  protected readonly edits = signal<Record<string, number>>({});
  protected readonly changed = computed(() => Object.keys(this.edits()).length > 0);
  protected readonly cellText = cellText;
  protected readonly path = computed(() => this.db.info()?.path ?? '');
  protected readonly groups = computed(() =>
    GROUPS.map(group => ({ ...group, pragmas: this.pragmas().filter(pragma => pragma.scope === group.scope) })).filter(group => group.pragmas.length > 0),
  );

  constructor() {
    // The server's state can change under this tab: Write Changes, a PRAGMA run in Execute SQL.
    effect(() => {
      this.db.revision();
      untracked(() => void this.load());
    });
  }

  protected value(pragma: PragmaInfo): number {
    return this.edits()[pragma.name] ?? Number(pragma.value);
  }

  protected edit(pragma: PragmaInfo, value: number): void {
    this.edits.update(edits => {
      const next = { ...edits };
      if (value === Number(pragma.value) || Number.isNaN(value)) {
        delete next[pragma.name];
      } else {
        next[pragma.name] = value;
      }
      return next;
    });
  }

  protected async load(): Promise<void> {
    try {
      this.pragmas.set((await this.db.pragmas()).pragmas);
      this.edits.set({});
    } catch (failure) {
      this.notice.error(failure);
    }
  }

  protected async save(): Promise<void> {
    try {
      for (const [name, value] of Object.entries(this.edits())) {
        await this.db.setPragma(name, value);
      }
      this.notice.info('Pragmas saved.');
    } catch (failure) {
      this.notice.error(failure);
      await this.load();
    }
  }
}
