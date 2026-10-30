# split-layout

VS Code style resizable panes for Angular + Material (M3 tokens, signals, zoneless-safe). No application dependencies:
copy this folder into another project and import `SplitComponent` and `SplitPaneComponent`.

```html
<ui-split orientation="horizontal">           <!-- or "vertical" to stack panes -->
  <ui-split-pane title="Explorer" icon="folder" [size]="240" [minSize]="160">
    <button paneActions mat-icon-button>…</button>   <!-- extra header buttons, left of Maximize -->
    …content…
  </ui-split-pane>
  <ui-split-pane>                              <!-- no size: takes the remaining space -->
    <my-tabs paneTitle />                      <!-- replaces the plain title text -->
    …content…
  </ui-split-pane>
  <ui-split-pane title="Details" [size]="400" headerless />
</ui-split>
```

Nest a `ui-split` inside a pane to build a grid (columns of rows or the other way round).

## Behaviour
- Drag the sash between two panes with the mouse or touch; the pair shares its combined width and stops at each pane's
  `minSize`. Arrow keys on the focused sash move it 16 px (Shift: 64 px); double click or Enter restores the initial size.
- The Maximize button (or a double click on the header) makes a pane fill the whole split; the others are hidden, not
  destroyed, so their state survives. Esc or the Restore button brings the layout back as it was.
- When the container shrinks, fixed-size panes give way from the end first; the flexible pane keeps its `minSize`.

## API
`ui-split`: `orientation` (`horizontal` | `vertical`), output `maximizedChange` (the pane or null), `toggleMaximize(pane)`, `restore()`.

`ui-split-pane`: `title`, `icon`, `size` (px; omit for the flexible pane), `minSize` (px, default 120), `maximizable`
(default true), `headerless`. Give the host a height if the split itself is not in a sized container; the split fills
its parent (`width/height: 100%`). Colours come from `--mat-sys-*`; override `--ui-split-header-background` and
`--ui-split-pane-background` (the card), `--ui-split-background` (the gutter) and `--ui-split-radius` to taste.
