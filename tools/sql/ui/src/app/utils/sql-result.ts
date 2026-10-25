import { ResultSetView } from '../bufs';
import { SqlReply, SqlResult } from '../models/api.models';

/** A JSON reply as a SqlResult. */
export function fromReply(reply: SqlReply): SqlResult {
  return {
    columns: reply.columns,
    rowCount: reply.rows.length,
    totalRows: reply.rowCount,
    truncated: reply.truncated,
    changes: reply.changes,
    lastInsertId: reply.lastInsertId,
    elapsedMs: reply.elapsedMs,
    inTransaction: reply.inTransaction,
    cell: (row, column) => reply.rows[row][column],
  };
}

/** A libbufs reply as a SqlResult; cells are read from the received bytes when the grid asks for them. */
export function fromBufs(view: ResultSetView, elapsedMs: number, inTransaction: boolean): SqlResult {
  // A run cut at maxRows holds fewer rows than it produced.
  return {
    columns: view.columns,
    rowCount: view.rowCount,
    totalRows: Math.max(view.totalRows, view.rowCount),
    truncated: view.truncated,
    changes: view.changes,
    lastInsertId: view.lastInsertId,
    elapsedMs,
    inTransaction,
    cell: (row, column) => view.cell(row, column),
  };
}
