import { AfterViewInit, Directive, ElementRef, inject } from '@angular/core';

/** Focuses its input when it appears and selects what it holds. */
@Directive({ selector: '[appAutofocus]' })
export class AutofocusDirective implements AfterViewInit {
  private readonly element = inject<ElementRef<HTMLInputElement>>(ElementRef);

  ngAfterViewInit(): void {
    this.element.nativeElement.focus();
    this.element.nativeElement.select();
  }
}
