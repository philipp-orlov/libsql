# The `sql` tool

`sql` is a command line for a libsql database: a prompt, `-c` and `-f` for scripts, and `--listen` for a
browser front end that works like [DB Browser for SQLite](https://sqlitebrowser.org/).

```
sql catalog.db --listen 8080
```

Open `http://127.0.0.1:8080/`. The window has the four tabs of DB Browser:

| Tab | What it does |
| --- | --- |
| Database Structure | Lists the tables, indices, views and triggers with their columns. Creates and deletes them, and modifies a table (rename it, rename, drop or add a column). Each dialog shows the SQL before it runs. Export SQL downloads a script that rebuilds the schema and the rows. |
| Browse Data | Shows a table or view in a grid. Sort by a column heading (`Ctrl`+click adds a column to the sort), filter in the box under it, page through the rows. Double-click a cell to edit it, or open the Edit Cell panel to see and edit a long value. Add and delete records, set a cell to NULL. Export CSV saves the rows that match the filter. |
| Edit Pragmas | Shows the pragmas that change what libsql does, in three groups: saved in the database file, this connection only, and reported by the store. |
| Execute SQL | Runs SQL from editor tabs. The editor colors the SQL and completes keywords, functions, tables and columns (`Ctrl+Space`, or as you type; `alias.` and `table.` offer columns). Runs the selection when there is one; `Ctrl+Enter` runs. Opens and saves `.sql` files, and saves the rows as CSV. The rows show in the same grid, sorted, filtered and grouped in the browser. |

Both grids (`ui/src/app/shared/data-grid`) work like Kendo UI's: drag a column heading into the bar above
the grid to group the rows by it, drag more to nest groups, click a group chip to turn its order around
and drag it away to ungroup. Drag headings to rearrange the columns and their right edge to resize them.
The menu of a heading sorts, groups, and shows or hides columns. In Browse Data the server sorts by the
grouped columns first, so a group continues on the next page.

The top bar shows which database the page edits and whether it is read-write or read-only.

## Options

| Option | Meaning |
| --- | --- |
| `--listen PORT` | Serve the front end on PORT, after any `-c` or `-f`. The tool then waits for Ctrl-C. |
| `--host ADDR` | Address to bind to. The default is `127.0.0.1`. |
| `--public DIR` | Serve `index.html` and the other files of DIR instead of the page built into the binary. |
| `--readonly` | Open the database read-only. The front end turns editing off. |
| `--token T` | With `--listen`: the database needs T. With a `ws://` URL: the token to present. |
| `--auth VALUE` | With a `ws://` URL: sent as the `Authorization` header, e.g. `"Bearer abc"`. |
| `--reports DIR` | With `--listen`: keep report definitions (one JSON file each) in DIR. The default is `<database file>.reports`. |
| `--ws-path P` | With `--listen`: where the WebSocket route is served. The default is `/api/v1/ws/sql`, which is where the page looks, so move it only for other clients. |
| `--tls-cert F`, `--tls-key F` | With `--listen`: serve https and wss on PORT with this certificate and key (PEM). Needs OpenSSL at build time. |
| `--ca FILE` | With a `wss://` URL: trust the certificates in FILE instead of the system's. |
| `--insecure` | With a `wss://` URL: accept any certificate. For tests only. |

## Reports

The tabs **Reports** and **Report Designer** run and build reports: parameters, SQL datasets, grouped tables with subtotals, interactive charts (bars,
stacked, horizontal, lines, areas, pies, donuts: tooltips, highlight across charts, legend toggles), sortable tables with foldable groups, a page header and
footer, and **PDF**, **Excel**, **Word** and **CSV** export, cross-tabs, columns, pictures, rich text, drill-through and sub-reports of what is shown, with no libraries behind it. The designer has a data pane to drag fields from, a chart gallery, undo/redo,
and edits an item when you click it in the preview. `/reports` is the runner on a page of its own, for
customers. Reports are JSON files in `<database>.reports/` (or `--reports DIR`) and run on a read-only session. See [../../docs/reports.md](../../docs/reports.md);
[../../demo](../../demo) has a database and six reports to try.

```
sql ../../demo/sales_demo.sqldb --listen 8080
```

The report features come with `SQL_WITH_REMOTE`; `-DSQL_WITH_REPORTS=OFF` builds the tool without them (no report code, routes, `/reports` page or
embedded font, `--reports` refused, the two tabs hidden).

## Remote

With `--listen` the tool serves the database over one WebSocket route (`/api/v1/ws/sql`,
`sql::remote::SqlSocket`, sql-remote.md). **That route is the only way in**: the page uses it for every tab, and
other programs reach it with `sql::Database::connect`, and so does the tool itself: give it a URL instead of a file.
There is no JSON API.

```
sql catalog.db --listen 8080 --token s3cret
sql ws://127.0.0.1:8080/api/v1/ws/sql --token s3cret -c "SELECT count(*) FROM orders"
echo ".tables" | sql ws://127.0.0.1:8080/api/v1/ws/sql --token s3cret
sql catalog.db --listen 8443 --tls-cert cert.pem --tls-key key.pem     # https and wss
sql wss://db.example:8443/api/v1/ws/sql --ca cert.pem -c ".tables"
```

Every session shares the one database handle, so a change made through one is pending for all and Write Changes
(commit) writes it. Rows arrive as libbufs batches that the page reads in place, and a running statement can be
stopped. A session that asks to be read-only (or `--readonly`) gets a reader of its own and cannot write.

`--token` protects the database: the page itself is public (it has to load to ask for the token), and when the
server wants one the page shows a field for it and keeps the token in the tab's `sessionStorage`. Without
`--token` anyone who can reach the address can change the data, so keep the default loopback address unless the
network is trusted. On loopback only this page's own origin may open the socket.

## Pending changes

An edit does not change the file at once. The first edit opens a transaction and the transaction stays
open until you press **Write Changes** (`Ctrl+S`) or **Revert Changes**, as in DB Browser. A statement in
Execute SQL that changes nothing leaves nothing pending. `BEGIN`, `COMMIT`, `PRAGMA` and the other
statements that control transactions run as written.

The tool has one database handle. Every browser that connects sees the same pending changes.

## Pragmas

A pragma is a setting. The Edit Pragmas tab says where each one lives:

- **Saved in the database file:** `user_version` and `application_id`. Every program that opens the file sees them.
- **This connection only:** `foreign_keys`, `recursive_triggers`, `ignore_check_constraints`, `temp_store` and
  `busy_timeout`. They belong to the connection of this tool. They are lost when the tool stops, and they do not
  change isvc or any other program that opens the file.
- **Reported by the store:** `journal_mode`, `encoding`, `page_size`, `page_count`, `freelist_count` and
  `schema_version`. They cannot be changed.

`journal_mode` always reads `wal`. libsql reports that name because SQLite tools ask for it. The store is a
copy-on-write B+tree: a commit writes new pages and then switches to them, so there is no journal and no WAL file.
`PRAGMA cache_size` and `PRAGMA reverse_unordered_selects` are accepted by libsql but have no effect, so the tab
does not list them.

## Safety

The interface has no login. Anyone who can reach the port can read and change the data.

- The tool binds to `127.0.0.1`. Use `--host` only on a network you trust, and expect a warning when you do.
- On `127.0.0.1` the server refuses a request with another `Host` name, so a web page cannot reach it
  through DNS rebinding.
- Every `POST` must have `Content-Type: application/json`. A form on another web site cannot send that.
- Names of tables and columns come back from the database and are quoted. Filter text is bound as a
  value.

## Limits

- Execute SQL shows the first 5000 rows of a result and says so.
- A cell of type BLOB shows its size and the first 32 bytes. The grid does not edit a blob.
- An INTEGER that a JavaScript number cannot hold is sent as `{"type":"integer","value":"..."}`.
- All requests that touch the database run one after another on one thread. A long query holds the
  others.
- Export CSV in Browse Data stops at 100000 rows.
- Another process cannot open a store while the tool has it open (and the other way round): the store allows one
  process. Stop isvc before you point the tool at its `metrics.db`.

## The Visualize tab

A debugging view of the store under the database: libnosql keeps everything in one file of fixed-size pages, and this
tab draws it.

- **Counters** of the file and the transactions, and a bar that divides the pages by what they are.
- **Page map**: every page of the file as a square, in file order, coloured as meta, branch, leaf, overflow or free
  (free pages are hatched: their bytes are stale). Pick a tree on the left and the others fade. Hover for the page's
  number, kind and tree; click to open it. A file with more than 40,000 pages shares a square between several.
- **Tree**: a table's or index's B+tree opened level by level, as a list or a diagram. Each page shows how full it
  is, how many entries lie below it and the separator key it is filed under (a rowid, or a decoded index key).
- **Page inspector**: the header; the stored hash against the one computed (a damaged page says so); how the page's
  4096 bytes are used (header, slot array, free gap, nodes, shared key prefix, hash); every node with its key and
  value decoded (rowids, key-encoded index entries, stored rows), links to child pages and to the overflow run a large
  value lives on; and a hex dump with each part coloured, scrolling to the node you select.
- **Meta pages**: the two alternating meta pages, which is current, their checksums, and the commit chain.
- **Free list**: the pages waiting for reuse and the transaction that retired them.
- **Check integrity** recomputes every page's hash and runs the structural check; bad pages are circled on the map.

All of it is read over the same SQL socket as the other tabs, from `PRAGMA storage_*`, which you can also use
yourself (committed state only; they never show a transaction's pending changes):

| Pragma | Rows |
| --- | --- |
| `storage_stats` | `name, value`: the store's counters |
| `storage_meta` | one per meta page: ids, checksums, transaction, page counts |
| `storage_trees` | one per tree: the table or index it holds, root, depth, page counts, entries |
| `storage_map` | runs of consecutive pages of one kind and tree |
| `storage_free` | free-list entries: the transaction that retired the pages, how many |
| `storage_page(n)` | page n's header, free space, shared prefix, stored and computed hash |
| `storage_nodes(n)` | the keys and values, or child pages, on page n |
| `storage_bytes(n)` | the page's bytes (an overflow run: its first 16 pages) |
| `storage_check` | `result, page, detail, pages_checked`: every page's hash, then the structural check |

They are also table-valued functions, so they join: `SELECT object, leaf_pages FROM pragma_storage_trees() ORDER BY
leaf_pages DESC`. `storage_row(blob)`, `storage_key(blob [, with_rowid])` and `storage_rowid(blob)` decode the bytes of
a stored row, an index or WITHOUT ROWID key, or a rowid key.

## What the page asks

The page needs nothing from the server but the socket. `ui/src/app/services/catalog.ts` holds the SQL behind each
tab: the schema (`sqlite_schema`, and the server's Describe request for columns and indexes), a page of a table
(`SELECT ... LIMIT ... OFFSET`, with `rowid` or the key of a WITHOUT ROWID table for edits), inserts, updates and
deletes by key, the pragmas, and the dump. An edit opens a transaction that stays pending until Write Changes
(`COMMIT`) or Revert Changes (`ROLLBACK`); the server does this for requests that carry the page's "managed" hint.

A filter is text typed under a column: plain text finds rows that contain it, and a leading `=`, `<>`,
`!=`, `<`, `<=`, `>` or `>=` compares. `=NULL` and `<>NULL` test for NULL.

## Build

The tool needs [libhttp](../../../libhttp) for `--listen`. CMake finds it as a sibling of libsql, or at
`-DSQL_HTTP_DIR=...`, or from a parent project that already has the `http` target. Without it the tool
builds without `--listen`. `-DSQL_TOOL_WEB=OFF` leaves the web part out.

`public/index.html` is the built front end, one file with its script, style and fonts inside. The build
compresses it and links it into the binary with `objcopy` (GNU binutils and zlib, see
[cmake/EmbedIndexHtml.cmake](cmake/EmbedIndexHtml.cmake)), so the binary needs no other file. Without them,
use `--public`.

```
cmake -S ../.. -B build && cmake --build build --target sql_cli
build/tools/sql/sql catalog.db --listen 8080
```

## The front end

The front end is an Angular and Angular Material application in [ui/](ui/).

It has a light and a dark theme. The button at the right of the toolbar (also on the `/reports` page) chooses light, dark or the browser's own
setting (the default); the choice is kept in the browser. A report's page is paper and stays white in either theme.

```
cd ui
npm install
npm run b          # builds, and writes ../public/index.html
npm start          # dev server on :4200, proxies the WebSocket route to 127.0.0.1:8080
npm test           # Karma and Jasmine in headless Chrome; set CHROME_BIN if Chrome is not on the PATH
```

Run `npm run b` after a change to the front end, then build the tool again. `public/index.html` is part
of the repository, so a build of the tool needs no Node.js.

## Tests

`ctest` (with `SQL_BUILD_TESTS=ON`) runs:

- `web_server`: the server on a loopback socket: the route, its token, origin and `Host` checks, read-only
  sessions, a moved route, the built-in page and `--public`, and that no JSON API answers.
- `ui_reports_unit` (when Node.js and the UI's packages are there): the report engine without a browser: the font reader and subsetter, the PDF
  writer's structure, layout and pagination, groups and totals, charts, expressions, formats, CSV, SQL parameter binding, definition checks.
- `web_ui_reports`, `web_ui_demo`, `web_ui_interactive` (when a browser is installed, and `SQL_WITH_REPORTS`): in headless Chrome, a customer runs a report,
  sees it, exports PDF and CSV; a report that tries to write is refused; the designer saves; `/reports` has no database tools and asks for the token; every
  report of the demo runs and exports; and with the real mouse: tooltips, highlight, selection, legend toggles, sorting, folding, and in the designer click-to-edit,
  the chart gallery, undo and drag and drop.
- `web_ui_e2e` (when Node.js and the UI's packages are there): the front end's data layer, bundled with esbuild,
  against the real binary: schema, browse with sort and filters, edits pending until written, WITHOUT ROWID
  tables, large integers and blobs, pragmas and the dump.

`npm test` runs the front end specs.

## Files

```
src/sql_tool.cpp     command line
src/web_server.cpp   libhttp routes, the SQL socket on one worker, front end delivery, the report routes
src/reports.cpp      the report files on disk
include/             headers of the web part
ui/                  Angular front end
public/index.html    the built front end
fonts/               the report font (Liberation Sans, SIL Open Font License), embedded in the binary
cmake/               EmbedIndexHtml.cmake
tests/               back end tests
```
