# Reports

The `sql` tool's web page has a reporting module in the spirit of SQL Server Reporting Services, much lighter: **a designer**
to build a report, **a runner** for the people who only read them, **PDF** and **CSV** export, and **no libraries** behind
it (the PDF writer, the font reader and the layout engine are in `tools/sql/ui/src/app/reports/core`, plain TypeScript).

```
sql demo/sales_demo.sqldb --listen 8080          # the demo: six reports, 150 customers, 1,100 order lines
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
and with the columns the table shows, a byte order mark in front so spreadsheets read the UTF-8.

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
| Text | One or more lines. May hold `{{ expressions }}`. Font (regular or bold), size, color, alignment, a background color (a banner). Wraps at the page width. |
| Table | The rows of a dataset: columns (field or expression, heading, relative width, alignment, number or date format, a total), groups (heading, order, subtotals, page break), striping, a grand total. The heading row repeats on every page; rows never split across pages. |
| Chart | Drawn from a dataset: a category column and one or more series. **Bars** (clustered, **stacked**, or **horizontal** for long names), **lines**, **areas**, **pies** and **donuts** (with the total in the hole); optional values on the marks, legend, title, height, number format, series colors (Tableau's palette by default). Vector shapes, so sharp at any zoom and in the PDF. |
| Line, Space, Page break | A rule across the page, empty room, a new page. |

The page header and footer hold text, lines and spaces and are drawn on every page; there `PageNumber` and `PageCount` are known.

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

[demo/](../demo) has a fictional B2B sales database and six reports on it: an executive dashboard (KPIs, five charts), revenue by region and
country, top customers, product performance, one customer's statement, and a sales-team leaderboard against quota.

## Not there yet

Sub-reports, drill-through, images, rich text inside one text box, cross-tab (matrix) tables, columns side by side on a page, per-report
permissions, scheduled or e-mailed runs, and export to Excel or Word. The definition format has room for them.
