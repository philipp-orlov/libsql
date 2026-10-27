import { Component, provideZonelessChangeDetection, signal } from '@angular/core';
import { TestBed } from '@angular/core/testing';

import { SplitComponent, SplitPaneComponent } from './index';

@Component({
  imports: [SplitComponent, SplitPaneComponent],
  template: `
    <div [style.width.px]="width()" [style.height.px]="500" style="display: block">
      <ui-split [orientation]="orientation()" (maximizedChange)="events.push($event?.title() ?? null)">
        <ui-split-pane title="One" [size]="200" [minSize]="100"><p class="one">one</p></ui-split-pane>
        <ui-split-pane title="Two" [minSize]="100"><p class="two">two</p></ui-split-pane>
        <ui-split-pane title="Three" [size]="150" [minSize]="100" [maximizable]="false"><p class="three">three</p></ui-split-pane>
      </ui-split>
    </div>
  `,
})
class HostComponent {
  readonly width = signal(900);
  readonly orientation = signal<'horizontal' | 'vertical'>('horizontal');
  readonly events: (string | null)[] = [];
}

describe('SplitComponent', () => {
  async function setup() {
    TestBed.configureTestingModule({ providers: [provideZonelessChangeDetection()] });
    const fixture = TestBed.createComponent(HostComponent);
    document.body.appendChild(fixture.nativeElement);
    await fixture.whenStable();
    const root: HTMLElement = fixture.nativeElement;
    const panes = () => Array.from(root.querySelectorAll<HTMLElement>('ui-split-pane'));
    const widths = () => panes().map((p) => Math.round(p.getBoundingClientRect().width));
    return { fixture, root, panes, widths };
  }

  function drag(sash: HTMLElement, dx: number): void {
    const box = sash.getBoundingClientRect();
    const x = box.left + box.width / 2;
    const y = box.top + 20;
    const init = { pointerId: 1, button: 0, bubbles: true, clientY: y };
    sash.dispatchEvent(new PointerEvent('pointerdown', { ...init, clientX: x }));
    sash.dispatchEvent(new PointerEvent('pointermove', { ...init, clientX: x + dx / 2 }));
    sash.dispatchEvent(new PointerEvent('pointermove', { ...init, clientX: x + dx }));
    sash.dispatchEvent(new PointerEvent('pointerup', { ...init, clientX: x + dx }));
  }

  it('gives fixed panes their size and the rest to the flexible pane', async () => {
    const { widths, fixture } = await setup();
    expect(widths()).toEqual([200, 550, 150]);
    fixture.destroy();
  });

  it('draws a sash after every pane but the last', async () => {
    const { root, fixture } = await setup();
    expect(root.querySelectorAll('.sash').length).toBe(2);
    fixture.destroy();
  });

  it('moves a sash with the mouse and keeps the total', async () => {
    const { root, widths, fixture } = await setup();
    drag(root.querySelectorAll<HTMLElement>('.sash')[0], 80);
    await fixture.whenStable();
    expect(widths()).toEqual([280, 470, 150]);
    drag(root.querySelectorAll<HTMLElement>('.sash')[1], -60);
    await fixture.whenStable();
    expect(widths()).toEqual([280, 410, 210]);
    fixture.destroy();
  });

  it('stops a sash at the neighbour minimum', async () => {
    const { root, widths, fixture } = await setup();
    drag(root.querySelectorAll<HTMLElement>('.sash')[0], -500);
    await fixture.whenStable();
    expect(widths()[0]).toBe(100);
    drag(root.querySelectorAll<HTMLElement>('.sash')[0], 2000);
    await fixture.whenStable();
    expect(widths()).toEqual([650, 100, 150]);
    fixture.destroy();
  });

  it('moves a sash with the arrow keys and resets on double click', async () => {
    const { root, widths, fixture } = await setup();
    const sash = root.querySelectorAll<HTMLElement>('.sash')[0];
    sash.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true }));
    await fixture.whenStable();
    expect(widths()[0]).toBe(216);
    sash.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
    await fixture.whenStable();
    expect(widths()).toEqual([200, 550, 150]);
    fixture.destroy();
  });

  it('maximises a pane, hides the others and restores them', async () => {
    const { root, panes, widths, fixture } = await setup();
    drag(root.querySelectorAll<HTMLElement>('.sash')[0], 40);
    await fixture.whenStable();
    root.querySelectorAll<HTMLElement>('.maximize')[0].click();
    await fixture.whenStable();
    expect(widths()).toEqual([900, 0, 0]);
    expect(panes()[0].classList).toContain('maximized');
    expect(root.querySelectorAll('.sash').length).toBe(0);
    expect(root.querySelector('.two')).not.toBeNull('hidden panes keep their content alive');
    expect(fixture.componentInstance.events).toEqual(['One']);

    panes()[0].dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    await fixture.whenStable();
    expect(widths()).toEqual([240, 510, 150]);
    expect(fixture.componentInstance.events).toEqual(['One', null]);
    fixture.destroy();
  });

  it('offers no maximise button where the pane opts out', async () => {
    const { root, fixture } = await setup();
    expect(root.querySelectorAll('.maximize').length).toBe(2);
    fixture.destroy();
  });

  it('shrinks the fixed panes when the container gets too small', async () => {
    const { widths, fixture } = await setup();
    fixture.componentInstance.width.set(330);
    await fixture.whenStable();
    await new Promise((resolve) => setTimeout(resolve, 50));
    await fixture.whenStable();
    const w = widths();
    expect(w[0] + w[1] + w[2]).toBeLessThanOrEqual(330);
    expect(w[1]).toBeGreaterThanOrEqual(100);
    fixture.destroy();
  });

  it('stacks panes and drags vertically', async () => {
    const { root, panes, fixture } = await setup();
    fixture.componentInstance.orientation.set('vertical');
    await fixture.whenStable();
    expect(root.querySelector('ui-split')!.classList).toContain('vertical');
    const sash = root.querySelectorAll<HTMLElement>('.sash')[0];
    const height = () => Math.round(panes()[0].getBoundingClientRect().height);
    const before = height();
    const box = sash.getBoundingClientRect();
    const init = { pointerId: 1, button: 0, bubbles: true, clientX: box.left + 10 };
    sash.dispatchEvent(new PointerEvent('pointerdown', { ...init, clientY: box.top + 2 }));
    sash.dispatchEvent(new PointerEvent('pointermove', { ...init, clientY: box.top + 32 }));
    sash.dispatchEvent(new PointerEvent('pointerup', { ...init, clientY: box.top + 32 }));
    await fixture.whenStable();
    expect(height()).toBe(before + 30);
    fixture.destroy();
  });
});
