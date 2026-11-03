import { bootstrapApplication } from '@angular/platform-browser';

import { appConfig } from './app/app.config';
import { ShellComponent } from './app/components/shell/shell.component';
import { ReportsPageComponent } from './app/reports/page/reports-page.component';
import { applyTheme, storedTheme } from '@ui-kit/core/theme';

import { THEME_KEY } from './app/app.config';

// the saved theme goes on before the first paint
applyTheme(storedTheme(THEME_KEY));

// /reports is the report runner alone, for people who should not see the database tools.
const runnerOnly = location.pathname === '/reports' || location.pathname === '/reports/';

bootstrapApplication(runnerOnly ? ReportsPageComponent : ShellComponent, appConfig).catch(error => console.error(error));
