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
| Browse Data | Shows a table or view in a grid. Sort by a column heading, filter in the box under it, page through the rows. Double-click a cell to edit it, or open the Edit Cell panel to see and edit a long value. Add and delete records, set a cell to NULL. Export CSV saves the rows that match the filter. |
| Edit Pragmas | Shows the pragmas that change what libsql does, in three groups: saved in the database file, this connection only, and reported by the store. |
| Execute SQL | Runs SQL from editor tabs. The editor colors the SQL and completes keywords, functions, tables and columns (`Ctrl+Space`, or as you type; `alias.` and `table.` offer columns). Runs the selection when there is one; `Ctrl+Enter` runs. Opens and saves `.sql` files, and saves the rows as CSV. |

The top bar shows which database the page edits and whether it is read-write or read-only.

## Options

| Option | Meaning |
| --- | --- |
| `--listen PORT` | Serve the front end on PORT, after any `-c` or `-f`. The tool then waits for Ctrl-C. |
| `--host ADDR` | Address to bind to. The default is `127.0.0.1`. |
| `--public DIR` | Serve `index.html` and the other files of DIR instead of the page built into the binary. |
| `--readonly` | Open the database read-only. The front end turns editing off. |

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

## The API

The front end uses a JSON API ([include/web_api.hpp](include/web_api.hpp)). Every reply has
`inTransaction`. An error has a 4xx or 5xx status and `{"error": "..."}`.

| Request | Body | Reply |
| --- | --- | --- |
| `GET /api/info` | | `path`, `version`, `readOnly`, `functions` |
| `GET /api/schema` | | `tables`, `views`, `indexes`, `triggers` |
| `GET /api/dump` | | `sql`, a script that rebuilds the schema and the rows |
| `POST /api/sql` | `sql`, `maxRows` | `columns`, `rows`, `rowCount`, `truncated`, `changes`, `lastInsertId`, `elapsedMs` |
| `POST /api/browse` | `table`, `offset`, `limit`, `sort`, `filters` | `columns`, `rows`, `keys`, `total`, `editable`, `view` |
| `POST /api/rows/insert` | `table`, `values` | `changes`, `lastInsertId` |
| `POST /api/rows/update` | `table`, `key`, `set` | `changes` |
| `POST /api/rows/delete` | `table`, `keys` | `changes` |
| `POST /api/commit` | `{}` | |
| `POST /api/revert` | `{}` | |
| `GET /api/pragmas` | | `pragmas`: `name`, `kind`, `scope`, `description`, `readOnly`, `value`, `choices` |
| `POST /api/pragmas` | `name`, `value` | |

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

```
cd ui
npm install
npm run b          # builds, and writes ../public/index.html
npm start          # dev server on :4200, proxies /api to 127.0.0.1:8080
npm test           # Karma and Jasmine in headless Chrome; set CHROME_BIN if Chrome is not on the PATH
```

Run `npm run b` after a change to the front end, then build the tool again. `public/index.html` is part
of the repository, so a build of the tool needs no Node.js.

## Tests

`ctest` (with `SQL_BUILD_TESTS=ON`) runs two programs for the back end:

- `web_api` calls the API directly: browse, filters, edits, pending changes, cell types, pragmas.
- `web_server` starts the server on a loopback socket: routing, the content-type and `Host` checks, the
  built-in page and `--public`.

`npm test` runs the front end specs.

## Files

```
src/sql_tool.cpp     command line
src/web_api.cpp      the JSON API on one sql::Database
src/web_server.cpp   libhttp routes, worker thread, front end delivery
include/             headers of the two web files
ui/                  Angular front end
public/index.html    the built front end
cmake/               EmbedIndexHtml.cmake
tests/               back end tests
```
