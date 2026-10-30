# Reports

The `sql` tool's web page has a reporting module in the spirit of SQL Server Reporting Services, much lighter: **a designer**
to build a report, **a runner** for the people who only read them, **PDF** and **CSV** export, and **no libraries** behind
it (the PDF writer, the font reader and the layout engine are in `tools/sql/ui/src/app/reports/core`, plain TypeScript).

```
sql demo/sales_demo.sqldb --listen 8080          # the demo: eleven reports, 150 customers, 1,100 order lines
http://127.0.0.1:8080/               the whole tool: Reports and Report Designer are tabs next to Execute SQL
http://127.0.0.1:8080/reports        the runner alone, for customers
```

The pages are **interactive**, not pictures: point at a bar, a point or a slice for its numbers, click a category to follow it through every chart,
switch series on and off in the legend, sort a table by a heading, fold its groups. (See *Interacting* below.)

A report is run in the browser: the page runs the report's queries on a **read-only session** of the server (a report cannot
change data), lays the result out into pages, shows them, and exports them. The server only stores the definitions.

## Using it

**Runner.** Choose a report, fill in its parameters, *View report*. Page through it (the page box, the arrows, PageUp/PageDown), zoom, *PDF* saves
the pages as they are shown, *CSV* saves the rows of a table of the report (a menu asks which when there are several): unformatted, in the order
and with the columns the table shows, a byte order mark in front so spreadsheets read the UTF-8. *Excel* and *Word* are described under *Export to Excel and Word*.

**Drill-through.** A chart, table or cross-tab whose definition has a `drill` opens another report when you click a bar, a row or a cell (the pointer
shows a hand and the card says *Click to open*). The target runs at once with the parameters the click gives it, and a trail above the title
(*Executive Sales Dashboard › Region Snapshot*) with a *Back* button leads back, to the report as it was run.

### Interacting

| You | What happens |
| --- | --- |
| Point at a bar, point, slice or a column of a chart | A card shows its numbers (every series of that category; a pie slice's value and share). The marks of that category stay and the rest fade, in **every chart that has the category**. |
| Click a mark | Selects the category: it stays highlighted across charts, a chip in the toolbar says which. Click it again, the chip's ✕ or Esc to clear. |
| Click an entry of a legend | Switches that series (or slice) off, and on again. The scale follows; a pie's shares are of what is left. |
| Click a heading of a table | Sorts by it: ascending, descending, back to the report's order. Inside groups, the groups keep their order. |
| Click a group's heading | Folds it to the heading, which then shows the group's totals; click again to unfold. |
| *Reset view* | Undoes all of the above. |

What you changed is part of the report you export: the **PDF and the CSV are made from what is on screen** (a series you switched off is not in the PDF, a
table you sorted is sorted in both). Selection and hover are only for looking. Charts animate when a page is drawn (not with "reduce motion" set).

**Designer** (tab *Report Designer*; not offered by a read-only server). *Open* a report or give a new one a name. Three panes: **Data**, **Design**
and **Preview**, which follows every change.

- **Data** lists the datasets with their fields, marked as Tableau does: blue *abc* for text, a calendar for dates, green *#* for numbers, and the report's
  parameters. Drag a **field** onto a shelf of a chart or a table (below), or into a text (it becomes `{{[field]}}`).
- **The preview is an editor.** Point at an item and it is outlined; click it and its form opens on the Layout tab. Clicking a legend entry, a heading
  or a group in the preview still works as in the runner.
- **Chart items** have a gallery of seven kinds (bars, stacked, horizontal, lines, areas, pie, donut) and three shelves: *Category* (one mark per value)
  and *Series* (numbers to plot). Dropping a text field on *Series* is refused with the reason; so is a field of another dataset. **Table items** have
  *Groups* and *Columns* shelves; a dropped field becomes a column (or a group with subtotals).
- **Undo and redo** (buttons, Ctrl+Z, Ctrl+Shift+Z or Ctrl+Y) cover every edit, a hundred deep. **Ctrl+S** saves.
- Above the preview are the report's **parameters**: change one and the queries run again. The queries also run (after a moment) when you change one
  of them, so the preview does not drift; *Run queries* does it at once.

The tabs of the Design pane:

| Tab | What it holds |
| --- | --- |
| General | File name, title, description, paper size and orientation, margins. |
| Parameters | What is asked before the report runs: name, label, type (text, integer, real, date, boolean), default, required, a list of choices or a query that gives them. |
| Datasets | The SELECTs the report reads. An SQL editor with completion; *Run* shows the first rows and teaches the layout editor the columns. |
| Layout | The page header, the body and the page footer, as lists of items with a form each (below). |
| JSON | The definition as stored. Edit it directly if you prefer; it applies as soon as it is valid. |

*Save* writes the report to the server, *Save as…* copies it, *Delete* removes it. Typing SQL never runs it; the dataset's *Run* button shows its first rows
and teaches the data pane its fields.

## Items

| Item | What it draws |
| --- | --- |
| Text | One or more lines. May hold `{{ expressions }}`. Font (regular or bold), size, color, alignment, a background color (a banner). Wraps at the page width. With **Rich text** on, the markup below formats parts of it. |
| Table | The rows of a dataset: columns (field or expression, heading, relative width, alignment, number or date format, a total), groups (heading, order, subtotals, page break), striping, a grand total. The heading row repeats on every page; rows never split across pages. |
| Chart | Drawn from a dataset: a category column and one or more series. **Bars** (clustered, **stacked**, or **horizontal** for long names), **lines**, **areas**, **pies** and **donuts** (with the total in the hole); optional values on the marks, legend, title, height, number format, series colors (Tableau's palette by default). Vector shapes, so sharp at any zoom and in the PDF. |
| Cross-tab | A matrix: one field's values across the top, one or two fields down the side, one number in each cell, with row and column totals and a color scale. See *Cross-tabs*. |
| Picture | A PNG, JPEG, GIF, WebP or SVG kept in the report, or read from a text column of the database. See *Pictures*. |
| Columns | Items side by side, each column with its own width, optionally as a bordered *card* with an accent bar. See *Columns*. |
| Sub-report | Another report, run and laid out in place, once or once per row of a dataset. See *Sub-reports*. |
| Line, Space, Page break | A rule across the page, empty room, a new page. |

The page header and footer hold text, pictures, lines and spaces and are drawn on every page; there `PageNumber` and `PageCount` are known.

## Rich text

A text item with **Rich text** switched on reads inline markup (the designer has buttons for it):

| Markup | Gives |
| --- | --- |
| `[b]bold[/b]`, `[u]underline[/u]` | bold, underlined |
| `[color=#e15759]red[/color]` (`[c=…]`) | colored text |
| `[size=24]big[/size]` (`[s=…]`) | another size in points (4 to 120) |
| `[bg=#fff3cd]marked[/bg]` | a highlighter background |
| `[/]` | closes the innermost tag |
| a line starting with `- ` | a bullet (the wrapped lines hang under the text) |
| `\[` | a bracket that is only a bracket |

Tags nest, and a line takes the height of its biggest run, so a small caption, a big number and small print fit one text box (that is what the demo's
KPI tiles are). `{{ expressions }}` are evaluated first and what they give is never read as markup, so a value that happens to hold `[b]` is safe.
The PDF, Word and the preview show the same runs; in Excel the text is plain.

## Pictures

An image item's `src` is a `data:image/png|jpeg|gif|webp|svg+xml;base64,…` URI, so a report stays one self-contained file (the designer's *Choose a
picture* button reads a file, up to 700 KiB). `width` and `height` are in points (one alone keeps the proportions; none means the picture's own size, shrunk to
fit), `fitWidth` stretches it across the page or column, `align` and `caption` place it. The `src` may hold `{{ }}` parts, so a picture can come from the
database: `{{First(icon)}}` with `dataset` set shows the `icon` text column of that dataset (the demo's category icons). A picture may also stand in the page
header or footer. In the PDF, JPEG goes in untouched, PNG as pixels with its transparency, and SVG and the rest are drawn at three times their size from the
browser's canvas; Word gets PNG, JPEG and GIF as they are and the others as PNG.

## Columns

A columns item has up to six columns, each a list of items of its own (text, tables, charts, cross-tabs, pictures, sub-reports, even more columns, but no page
break). Each column is laid out in its share of the width (`width`, relative) and runs over to the next page by itself; the block ends under the tallest
column. A column with `card` is drawn as a white card with a thin border and rounded corners, `accent` adds a colored bar along its top, and cards of one
block are as tall as each other. In the designer the items of a column are edited right inside it, and a click on one in the preview opens it. In Word the
columns are a borderless table.

## Cross-tabs

A matrix item pivots a dataset: `rows` (one or two fields; the second nests in the first), `column`, and `value` aggregated by `agg` (`sum` by default,
`avg`, `min`, `max`, `count`). `rowTotals` and `columnTotals` add the totals, `subtotals` puts the totals of the first row field on its heading line,
`sortRows` (`data`, `asc`, `desc`, `total`) and `sortColumns` order the headings, `format` formats the cells, `title` and `corner` label it. The **color
scale** `heat` tints each cell from light to dark by its value (`blue` by default, `green`, `orange`, `red`, `purple`, `teal`, `diverging` for red below and blue above the
middle of the range, or `none`) and the text turns white where it needs to; a small legend shows the range. A cross-tab that is too wide for the page shows the
columns that fit and says so. It is interactive: the pointer shows the cell's numbers and lights up its row in every chart that has the same category; a click
on a column heading sorts the rows by that column.

## Drill-through

`drill` on a chart, table or cross-tab is `{"report": "Customer Statement", "params": {"customer": "{{customer_id}}"}}`. Each value is a text with `{{ }}`
parts evaluated for the clicked mark: for a chart the row of its dataset behind the bar, point or slice; for a table the row; for a cross-tab the row fields,
the column field and the value field of the cell. The report's own parameters are visible too (`{{year}}`). Parameters the target has and the click does not give keep
the target's default. Drilling needs the target to exist on the server; the designer offers the names.

## Sub-reports

A sub-report item runs another report and lays out its body in place, with its own datasets and parameters (its page setup, header and footer are not used).
Set `dataset` to run it once for each row (at most `limit`, default 12, 50 at most; `pageBreak` starts each on a new page); then `params` read the row:
`{"region": "{{region}}"}`. A parameter that is not mapped takes the same-named parameter of the report it sits in, else its default, so `year`
flows down by itself. The page fetches everything first (the target is loaded and its datasets run, recursively, at most three deep and 60 runs in all;
a report containing itself is refused), so a missing target, a failing query or too many rows come back as warnings above the pages. What the reader
changes in a sub-report (sort, fold, legend) works as anywhere else.

## Export to Excel and Word

*Excel* writes an `.xlsx` workbook: a first sheet *Report* (title, parameters, the texts, links to the other sheets) and one sheet for every table, cross-tab and chart.
Cells hold **numbers and dates**, not text, with their number formats (`#,##0.00`, `0.0%`, `yyyy-mm-dd`…), a styled heading row, group headings, subtotals and
the grand total as they are in the report, frozen headings, filters on plain tables, and the cross-tab's colors. A chart's sheet holds the numbers behind it.
What you sorted or switched off is reflected. *Word* writes a `.docx`: paragraphs with their bold, colors, sizes and bullets, tables and cross-tabs as real tables
(heading row repeated on every page, colors kept), pictures, charts as pictures, columns as a borderless table, sub-reports in place, the report title in the
header and *Page x of y* in the footer. Neither needs a library or a server: they are zip files written in the page.

## Expressions

Text, group headings and computed columns use `{{ expression }}` (computed columns take the expression itself). Expressions are
parsed and walked by the page; nothing is passed to `eval`.

| | |
| --- | --- |
| Values | numbers, `'text'` or `"text"`, `true`, `false`, `null` |
| Names | a column of the current row (`[Column with spaces]` in brackets), a report parameter, `ReportName`, `Now` (an ISO time), `PageNumber`, `PageCount` |
| Operators | `+ - * / %` (`+` joins text; dividing by zero gives NULL), `== != < > <= >=` (a single `=` also compares), `&& \|\| !`, `a ? b : c` |
| Aggregates | `Sum Avg Min Max Count First Last` over the rows of the *scope*: the group in a group heading, subtotal or column of a grouped table; the whole table otherwise; for a text item, the rows of its `dataset`. `Count()` counts rows, `Count(x)` the non-null values |
| Functions | `Format(value, pattern)`, `Upper`, `Lower`, `Trim`, `Len`, `Round(x, digits)`, `Abs`, `Iif(c, a, b)`, `Coalesce(a, b, …)`, `Substring(s, start, length)` |

A failing expression shows as `#Error` in the output and as a message in the designer. A table column `{expression: "revenue / Sum(revenue)"}`
is each row's share of its group.

**Formats.** A number pattern: `#,##0.00` (`0` always shows a digit, `#` only when needed, `,` groups thousands), `0.0%` (multiplies by 100),
`$#,##0`, any text around the digits is kept. A date pattern uses `yyyy MM dd HH mm ss` (`yyyy-MM-dd HH:mm`); text dates are read as UTC,
numbers as seconds, milliseconds or microseconds since 1970 by their size.

## Parameters and datasets

A dataset is one SELECT; `:name` (also `@name`, `$name`) is the report parameter of that name. The page rewrites them to `?1`, `?2` and binds the
values, so a parameter is never pasted into the SQL. A blank parameter is NULL, which makes optional filters easy:
`WHERE (:region IS NULL OR region = :region)`. Row counts are capped at 100,000 per dataset (the report says so); a report is cut at 2,000 pages.

## Files and the server

Each report is `<name>.json` in a directory next to the database file, `<database>.reports/` by default, or `--reports DIR`.
Names are 1 to 80 letters, digits, spaces, `_`, `-` and `.` (nothing that could leave the directory). The format is versioned (`"format": 1`),
readable and diff-friendly, so reports can be kept in version control and copied between servers.

| Request | |
| --- | --- |
| `GET /api/v1/reports` | `[{"name", "size", "modified"}, …]` |
| `GET /api/v1/reports/{name}` | the definition |
| `PUT /api/v1/reports/{name}` | stores it (`Content-Type: application/json`, valid JSON, up to 1 MiB) |
| `DELETE /api/v1/reports/{name}` | removes it |
| `GET /fonts/LiberationSans-{Regular,Bold}.ttf` | the report font, gzipped |
| `GET /reports` | the runner page |

With `--token` the `/api/v1/reports` routes need `Authorization: Bearer <token>` (the page sends the token it was given). With `--readonly`
reports can be listed, read and run, not saved or deleted, and the designer is not offered. Writes go through a temporary file and a rename.

## PDF

The writer produces PDF 1.4 by hand: pages of text, rectangles, lines and Bézier paths. Text is set in **Liberation Sans** (SIL Open Font
License, `tools/sql/fonts/`), embedded as a *subset* of the glyphs the report uses (a report is some 40 KiB), with a ToUnicode map so text can be
selected, searched and copied. The font covers Latin, Greek and Cyrillic; other scripts (CJK, Arabic, Hebrew, Thai) have no glyphs in it and
show as an empty box. Streams are compressed with the browser's `CompressionStream`. The on-screen preview draws the same primitives from the same
font with kerning off, so lines break at the same words in both.

## Building it out

The report features belong to the remote layer: they are built with **`SQL_WITH_REMOTE`** and can be left out with **`SQL_WITH_REPORTS=OFF`** while
keeping the rest of the remote layer. Without them the binary has no report code, no `/api/v1/reports` routes, no `/reports` page and no embedded
font (it is some 0.4 MB smaller), `--reports` is refused, and the page hides the two tabs (it asks the server whether it has reports). The page
itself is built once; it only shows what the server it talks to offers.

## Demo

[demo/](../demo) has a fictional B2B sales database and eleven reports on it: an executive dashboard (KPI tiles, five charts, a cross-tab, drill-through to regions
and categories), revenue by region and country, top customers (click for the statement), product performance, one customer's statement, a sales-team
leaderboard against quota, a region snapshot, a regional review that runs the snapshot as a sub-report for every region, a category card and a product catalog
that repeats it for every category with pictures read from the database, and a sales matrix of three cross-tabs.

## Not there yet

Per-report permissions, scheduled or e-mailed runs, hyperlinks to web addresses inside text, charts as native Excel charts, and sub-reports whose
parameters depend on each other across several runs. The definition format has room for them.

## Material only

Every control of the web UI is an Angular Material component (`mat-select`, `mat-autocomplete`, `mat-datepicker`, `mat-checkbox`, `matInput` in a `mat-form-field`, buttons, tabs, menus). The report editors use the wrappers in `reports/shared/` (`app-select`, `app-combo`, `app-date`, `app-text`). `node scripts/check-material.mjs` (ctest `ui_material_only`) fails when a template uses a native `<select>`, `<datalist>`, or an `<input>`/`<textarea>` without `matInput`; the only exceptions are `type="file"` and `type="color"`, for which Material has no component.
