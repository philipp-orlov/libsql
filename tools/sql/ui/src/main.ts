import { bootstrapApplication } from '@angular/platform-browser';

import { appConfig } from './app/app.config';
import { ShellComponent } from './app/components/shell/shell.component';

bootstrapApplication(ShellComponent, appConfig).catch(error => console.error(error));
