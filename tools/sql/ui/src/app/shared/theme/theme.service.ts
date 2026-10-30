// Copyright (c) 2026 Philipp Orlov

import { Injectable, signal } from '@angular/core';

export type ThemeChoice = 'system' | 'light' | 'dark';

const KEY = 'sql.theme';

export function storedTheme(): ThemeChoice {
  try {
    const v = localStorage.getItem(KEY);
    return v === 'light' || v === 'dark' ? v : 'system';
  } catch {
    return 'system';
  }
}

/** `html[data-theme]` picks the color scheme the Material theme follows; without it the browser's own setting decides. */
export function applyTheme(choice: ThemeChoice): void {
  if (choice === 'system') document.documentElement.removeAttribute('data-theme');
  else document.documentElement.setAttribute('data-theme', choice);
}

/** Light, dark or the system's setting, remembered in this browser. */
@Injectable({ providedIn: 'root' })
export class ThemeService {
  readonly choice = signal<ThemeChoice>(storedTheme());

  constructor() {
    applyTheme(this.choice());
  }

  set(choice: ThemeChoice): void {
    this.choice.set(choice);
    applyTheme(choice);
    try {
      if (choice === 'system') localStorage.removeItem(KEY);
      else localStorage.setItem(KEY, choice);
    } catch {
      // private window or blocked storage: the choice lasts until the page is closed
    }
  }
}
