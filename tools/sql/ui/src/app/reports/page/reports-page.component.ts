// Copyright (c) 2026 Philipp Orlov

import { Component, OnInit, inject } from '@angular/core';
import { FormsModule } from '@angular/forms';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatToolbarModule } from '@angular/material/toolbar';

import { DatabaseService } from '../../services/database.service';
import { NoticeService } from '../../services/notice.service';
import { ReportRunnerComponent } from '../runner/report-runner.component';
import { ReportService } from '../report.service';

/**
 * The runner on a page of its own (`/reports`): what customers get, without the database tools. The same server, the same
 * token; reports still run on a read-only session.
 */
@Component({
  selector: 'app-reports-page',
  imports: [MatInputModule, MatFormFieldModule, FormsModule, MatButtonModule, MatIconModule, MatToolbarModule, ReportRunnerComponent],
  template: `
    <div class="page">
      <mat-toolbar class="toolbar">
        <mat-icon>assessment</mat-icon>
        <span class="title">Reports</span>
        <span class="spacer"></span>
        <span class="path mono">{{ db.info()?.path }}</span>
      </mat-toolbar>
      @if (db.authRequired()) {
        <form class="token" (ngSubmit)="signIn(token.value)">
          <p>This server asks for a token.</p>
          <mat-form-field appearance="outline" subscriptSizing="dynamic" class="token-field"><mat-label>Token</mat-label><input matInput #token name="token" type="password" autocomplete="off" /></mat-form-field>
          <button mat-flat-button type="submit">Connect</button>
        </form>
      }
      @if (svc.available() === false) {
        <p class="off">This server was built without reports.</p>
      } @else {
        <app-report-runner class="runner" />
      }
    </div>
  `,
  styles: `
    .page {
      display: flex;
      flex-direction: column;
      height: 100vh;
    }
    .toolbar {
      flex: none;
      gap: 8px;
    }
    .spacer {
      flex: 1;
    }
    .path {
      font-size: 12px;
      color: var(--mat-sys-on-surface-variant);
    }
    .runner {
      flex: 1;
      min-height: 0;
    }
    .off {
      padding: 16px;
      color: var(--mat-sys-on-surface-variant);
    }
    .token {
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 8px 16px;
    }
  `,
})
export class ReportsPageComponent implements OnInit {
  protected readonly db = inject(DatabaseService);
  protected readonly svc = inject(ReportService);
  private readonly notice = inject(NoticeService);

  async ngOnInit(): Promise<void> {
    void this.svc.probe();
    try {
      await this.db.load();
    } catch (failure) {
      if (!this.db.authRequired()) this.notice.error(failure);
    }
  }

  protected async signIn(token: string): Promise<void> {
    try {
      await this.db.useToken(token.trim());
      this.svc.close();
      this.svc.revision.update(n => n + 1);
    } catch (failure) {
      if (!this.db.authRequired()) this.notice.error(failure);
    }
  }
}
