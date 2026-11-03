import { provideHttpClient } from '@angular/common/http';
import { ApplicationConfig, provideBrowserGlobalErrorListeners, provideZonelessChangeDetection } from '@angular/core';
import { THEME_STORAGE_KEY } from '@ui-kit/angular/theme';

/** Where the theme choice was kept before the ui-kit, so that nobody loses theirs. */
export const THEME_KEY = 'sql.theme';

export const appConfig: ApplicationConfig = {
  providers: [provideBrowserGlobalErrorListeners(), provideZonelessChangeDetection(), provideHttpClient(), { provide: THEME_STORAGE_KEY, useValue: THEME_KEY }],
};
