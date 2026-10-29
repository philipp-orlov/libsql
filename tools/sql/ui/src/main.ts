import { bootstrapApplication } from '@angular/platform-browser';

import { appConfig } from './app/app.config';
import { ShellComponent } from './app/components/shell/shell.component';
import { ReportsPageComponent } from './app/reports/page/reports-page.component';

// /reports is the report runner alone, for people who should not see the database tools.
const runnerOnly = location.pathname === '/reports' || location.pathname === '/reports/';

bootstrapApplication(runnerOnly ? ReportsPageComponent : ShellComponent, appConfig).catch(error => console.error(error));
