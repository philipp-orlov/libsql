import { Component, OnInit, computed, inject } from '@angular/core';
import { FormsModule } from '@angular/forms';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatTabsModule } from '@angular/material/tabs';
import { MatToolbarModule } from '@angular/material/toolbar';
import { MatTooltipModule } from '@angular/material/tooltip';

import { DatabaseService } from '../../services/database.service';
import { NoticeService } from '../../services/notice.service';
import { WorkspaceService } from '../../services/workspace.service';
import { BrowseComponent } from '../browse/browse.component';
import { ExecuteSqlComponent } from '../execute-sql/execute-sql.component';
import { PragmasComponent } from '../pragmas/pragmas.component';
import { StructureComponent } from '../structure/structure.component';

@Component({
  selector: 'app-shell',
  imports: [
    FormsModule,
    MatButtonModule,
    MatIconModule,
    MatTabsModule,
    MatToolbarModule,
    MatTooltipModule,
    StructureComponent,
    BrowseComponent,
    PragmasComponent,
    ExecuteSqlComponent,
  ],
  templateUrl: './shell.component.html',
  styleUrl: './shell.component.scss',
  host: { '(document:keydown)': 'onKeydown($event)', '(window:beforeunload)': 'onUnload($event)' },
})
export class ShellComponent implements OnInit {
  protected readonly db = inject(DatabaseService);
  protected readonly workspace = inject(WorkspaceService);
  private readonly notice = inject(NoticeService);

  protected readonly readOnly = computed(() => this.db.info()?.readOnly ?? false);

  async ngOnInit(): Promise<void> {
    try {
      await this.db.load();
    } catch (failure) {
      // A server that wants a token is not an error to report: the form below asks for it.
      if (!this.db.authRequired()) {
        this.notice.error(failure);
      }
    }
  }

  protected async signIn(token: string): Promise<void> {
    try {
      await this.db.useToken(token.trim());
    } catch (failure) {
      if (!this.db.authRequired()) {
        this.notice.error(failure);
      }
    }
  }

  protected async write(): Promise<void> {
    try {
      await this.db.commit();
      this.notice.info('Changes written.');
    } catch (failure) {
      this.notice.error(failure);
    }
  }

  protected async revert(): Promise<void> {
    try {
      await this.db.revert();
      this.notice.info('Changes reverted.');
    } catch (failure) {
      this.notice.error(failure);
    }
  }

  /** Asks before the page is closed with changes waiting to be written. */
  protected onUnload(event: BeforeUnloadEvent): void {
    if (this.db.inTransaction()) {
      event.preventDefault();
    }
  }

  protected onKeydown(event: KeyboardEvent): void {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 's') {
      event.preventDefault();
      if (this.db.inTransaction()) {
        void this.write();
      }
    }
  }
}
