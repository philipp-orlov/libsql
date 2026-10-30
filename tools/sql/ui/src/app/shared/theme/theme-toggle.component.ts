// Copyright (c) 2026 Philipp Orlov

import { Component, computed, inject } from '@angular/core';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatMenuModule } from '@angular/material/menu';
import { MatTooltipModule } from '@angular/material/tooltip';

import { ThemeChoice, ThemeService } from './theme.service';

const OPTIONS: { value: ThemeChoice; label: string; icon: string }[] = [
  { value: 'system', label: 'System', icon: 'brightness_auto' },
  { value: 'light', label: 'Light', icon: 'light_mode' },
  { value: 'dark', label: 'Dark', icon: 'dark_mode' },
];

/** A toolbar button with a menu to choose the light, the dark or the system's theme. */
@Component({
  selector: 'app-theme-toggle',
  imports: [MatButtonModule, MatIconModule, MatMenuModule, MatTooltipModule],
  template: `
    <button mat-icon-button class="theme" [matMenuTriggerFor]="menu" matTooltip="Theme" aria-label="Theme">
      <mat-icon>{{ current().icon }}</mat-icon>
    </button>
    <mat-menu #menu="matMenu">
      @for (o of options; track o.value) {
        <button mat-menu-item class="theme-{{ o.value }}" [attr.aria-pressed]="o.value === theme.choice()" (click)="theme.set(o.value)">
          <mat-icon>{{ o.value === theme.choice() ? 'check' : o.icon }}</mat-icon>
          <span>{{ o.label }}</span>
        </button>
      }
    </mat-menu>
  `,
})
export class ThemeToggleComponent {
  protected readonly theme = inject(ThemeService);
  protected readonly options = OPTIONS;
  protected readonly current = computed(() => OPTIONS.find(o => o.value === this.theme.choice())!);
}
