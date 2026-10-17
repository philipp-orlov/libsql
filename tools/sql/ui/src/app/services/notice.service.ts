import { Injectable, inject } from '@angular/core';
import { MatSnackBar } from '@angular/material/snack-bar';

/** Short messages at the bottom of the window. */
@Injectable({ providedIn: 'root' })
export class NoticeService {
  private readonly snackBar = inject(MatSnackBar);

  info(message: string): void {
    this.snackBar.open(message, 'Close', { duration: 3000 });
  }

  error(failure: unknown): void {
    const message = failure instanceof Error ? failure.message : String(failure);
    this.snackBar.open(message, 'Close', { duration: 10000, panelClass: 'notice-error' });
  }
}
