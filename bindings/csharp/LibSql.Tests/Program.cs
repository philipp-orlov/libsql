// Copyright (c) 2026 Philipp Orlov

// Exercises the C# binding of libsql, on a local database and on one served by the sql tool (WebSocket and Unix socket).
//   dotnet run -- <work dir> [<sql tool>]          (SQL_LIBRARY names libsql_c)
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using LibSql;

int failures = 0, checks = 0;
void Check(bool ok, string what)
{
    checks++;
    if (!ok) { failures++; Console.Error.WriteLine($"  CHECK FAILED: {what}"); }
}
void Test(string name, Action body)
{
    try { body(); Console.WriteLine($"ok - {name}"); }
    catch (Exception e) { failures++; Console.Error.WriteLine($"not ok - {name}: {e}"); }
}
void Throws(ErrorCode code, Action body, string what, Constraint? constraint = null)
{
    try { body(); Check(false, $"{what}: nothing thrown"); }
    catch (SqlException e)
    {
        Check(e.Code == code, $"{what}: expected {code}, got {e.Code} ({e.Message})");
        if (constraint != null) Check(e.Constraint == constraint, $"{what}: expected constraint {constraint}, got {e.Constraint}");
    }
}

var work = args.Length > 0 ? args[0] : Path.Combine(Path.GetTempPath(), "sql-cs-" + Environment.ProcessId);
Directory.CreateDirectory(work);
foreach (var f in Directory.GetFiles(work)) File.Delete(f);

// What both kinds of database must do the same.
void Common(Database db, string kind)
{
    db.Exec("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, age INTEGER, score REAL, photo BLOB, born DATETIME)").Dispose();
    using (var r = db.Exec("INSERT INTO people (name, age, score, photo, born) VALUES (?, ?, ?, ?, ?)", "Ann", 31, 1.5, new byte[] { 1, 2, 3 }, new DateTime(1990, 5, 17, 0, 0, 0, DateTimeKind.Utc)))
        Check(r.Changes == 1 && r.LastInsertId == 1, $"{kind}: insert reports");
    db.Run("INSERT INTO people (name, age) VALUES (?, ?)", "Bob", null);
    db.Run("INSERT INTO people (name, age, score) VALUES (?, ?, ?)", "héllo ✓", 7L, -0.25);

    using (var r = db.Exec("SELECT id, name, age, score, photo, born FROM people ORDER BY id"))
    {
        Check(r.RowCount == 3 && r.ColumnCount == 6, $"{kind}: shape");
        Check(r.Columns.SequenceEqual(new[] { "id", "name", "age", "score", "photo", "born" }), $"{kind}: columns");
        var ann = r[0];
        Check(ann.GetInt64(0) == 1 && ann.GetString(1) == "Ann" && ann.GetInt64(2) == 31 && ann.GetDouble(3) == 1.5, $"{kind}: scalar cells");
        Check(ann.Cell("photo").Bytes.SequenceEqual(new byte[] { 1, 2, 3 }), $"{kind}: blob in place");
        Check(ann["born"] is DateTime d && d == new DateTime(1990, 5, 17, 0, 0, 0, DateTimeKind.Utc), $"{kind}: datetime");
        Check(r[1].IsNull(2) && r[1]["age"] == null && r[1]["photo"] == null, $"{kind}: nulls");
        Check(r[2].GetString(1) == "héllo ✓" && r[2].GetDouble(3) == -0.25, $"{kind}: unicode and negative real");
        Check(r.Rows.Select(x => x.GetString(1)).SequenceEqual(new[] { "Ann", "Bob", "héllo ✓" }), $"{kind}: enumeration");
    }

    Check((long)db.Scalar("SELECT count(*) FROM people")! == 3, $"{kind}: scalar");
    Check(db.Scalar("SELECT name FROM people WHERE age > ?", 100) == null, $"{kind}: empty result");
    Check(db.Run("UPDATE people SET age = age + 1 WHERE age IS NOT NULL") == 2, $"{kind}: update count");

    // prepared statements, positional and named
    using (var s = db.Prepare("SELECT name FROM people WHERE id = ?"))
    {
        Check(s.Parameters == 1 && !s.Writes, $"{kind}: statement shape");
        for (int id = 1; id <= 3; id++)
        {
            using var r = s.Exec(id);
            Check(r.RowCount == 1, $"{kind}: statement row {id}");
        }
    }
    using (var s = db.Prepare("INSERT INTO people (name, age) VALUES (:name, :age)"))
    {
        Check(s.Writes && s.ParameterIndex(":name") == 1, $"{kind}: named parameters");
        s.Bind(":name", "Cy").Bind(":age", 44).Exec().Dispose();
        s.Clear().Bind(":name", "Di").Exec().Dispose();
    }
    Check((long)db.Scalar("SELECT count(*) FROM people")! == 5 && db.Scalar("SELECT age FROM people WHERE name = 'Di'") == null, $"{kind}: statement inserts");

    // transactions
    using (var t = db.Begin())
    {
        t.Run("INSERT INTO people (name) VALUES ('temp')");
        using var r = t.Exec("SELECT count(*) FROM people");
        Check((long)r[0][0]! == 6, $"{kind}: sees its own write");
    }
    Check((long)db.Scalar("SELECT count(*) FROM people")! == 5, $"{kind}: dispose rolls back");
    using (var t = db.Begin())
    {
        t.Run("INSERT INTO people (name) VALUES ('kept')");
        t.Commit();
    }
    Check((long)db.Scalar("SELECT count(*) FROM people")! == 6, $"{kind}: commit keeps");
    using (var t = db.Begin(readOnly: true))
        Throws(ErrorCode.ReadOnly, () => t.Run("INSERT INTO people (name) VALUES ('no')"), $"{kind}: read-only transaction");

    // errors
    Throws(ErrorCode.SyntaxError, () => db.Exec("SELEC 1"), $"{kind}: syntax");
    Throws(ErrorCode.NoSuchTable, () => db.Exec("SELECT * FROM absent_table"), $"{kind}: no table");
    Throws(ErrorCode.ConstraintViolation, () => db.Run("INSERT INTO people (name) VALUES ('Ann')"), $"{kind}: unique", Constraint.Unique);
    Throws(ErrorCode.ConstraintViolation, () => db.Run("INSERT INTO people (name) VALUES (NULL)"), $"{kind}: not null", Constraint.NotNull);
    Check((long)db.Scalar("SELECT 1")! == 1, $"{kind}: usable after errors");

    // catalog
    Check(db.Tables().Contains("people"), $"{kind}: tables");
    var info = db.Table("people");
    Check(info.Columns.Count == 6 && info.Columns[0].PrimaryKey && info.Columns[1].NotNull && info.Columns[1].Unique && info.Columns[1].Name == "name", $"{kind}: table info");
    Check(info.Sql.Contains("CREATE TABLE"), $"{kind}: table sql");

    // many rows, big values
    db.Exec("CREATE TABLE bulk (id INTEGER PRIMARY KEY, body TEXT, data BLOB)").Dispose();
    using (var t = db.Begin())
    {
        using var insert = db.Prepare("INSERT INTO bulk (body, data) VALUES (?, ?)");
        var blob = new byte[4096];
        for (int i = 0; i < 2000; i++)
        {
            blob[0] = (byte)(i & 0xff);
            insert.Bind(1, $"row {i}").Bind(2, blob).Exec(t).Dispose();
        }
        t.Commit();
    }
    using (var r = db.Exec("SELECT id, body, data FROM bulk ORDER BY id"))
    {
        Check(r.RowCount == 2000, $"{kind}: 2000 rows");
        int bad = 0;
        for (int i = 0; i < r.RowCount; i++)
        {
            var row = r[i];
            if (row.GetString(1) != $"row {i}" || row.Cell(2).Bytes.Length != 4096 || row.Cell(2).Bytes[0] != (byte)(i & 0xff)) bad++;
        }
        Check(bad == 0, $"{kind}: every row intact");
    }
    var huge = new string('x', 256 << 10);
    Check(db.Scalar("SELECT length(?)", huge)! is 256L << 10, $"{kind}: a 256 KiB parameter");

    // interrupt a long statement from another thread
    var killer = Task.Run(() => { Thread.Sleep(200); db.Interrupt(); });
    Throws(ErrorCode.Interrupted, () => db.Exec("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT count(*) FROM c"), $"{kind}: interrupt");
    killer.Wait();
    Check((long)db.Scalar("SELECT 2")! == 2, $"{kind}: usable after interrupt");
}

Test("version and completeness", () =>
{
    Check(Database.Version.Length > 0, "version");
    Check(Database.IsComplete("SELECT 1;") && !Database.IsComplete("SELECT 1 +"), "complete");
});

Test("local database", () =>
{
    using var db = Database.Open(Path.Combine(work, "local.db"));
    Check(!db.IsRemote, "not remote");
    Common(db, "local");
});

Test("reopen keeps data, read-only refuses writes", () =>
{
    var path = Path.Combine(work, "reopen.db");
    using (var db = Database.Open(path, new OpenOptions { Durability = Durability.Lazy, SyncInterval = TimeSpan.FromMilliseconds(50) }))
    {
        db.Run("CREATE TABLE t (a INTEGER)");
        db.Run("INSERT INTO t VALUES (1), (2), (3)");
    }
    using var ro = Database.Open(path, new OpenOptions { ReadOnly = true });
    Check((long)ro.Scalar("SELECT sum(a) FROM t")! == 6, "data survived");
    Throws(ErrorCode.ReadOnly, () => ro.Run("INSERT INTO t VALUES (4)"), "read-only database");
    Throws(ErrorCode.Busy, () => { using var a = Database.Open(path); }, "second writer on a file", null);
});

Test("a reader handle on another thread", () =>
{
    using var db = Database.Open(Path.Combine(work, "readers.db"));
    db.Run("CREATE TABLE n (v INTEGER)");
    db.Run("INSERT INTO n VALUES (1)");
    var readers = Enumerable.Range(0, 4).Select(_ => Task.Run(() =>
    {
        using var rd = db.OpenReader();
        long last = 0;
        for (int i = 0; i < 200; i++) { var v = (long)rd.Scalar("SELECT max(v) FROM n")!; if (v < last) return false; last = v; }
        return true;
    })).ToArray();
    for (int i = 2; i <= 100; i++) db.Run("INSERT INTO n VALUES (?)", i);
    Check(Task.WhenAll(readers).Result.All(x => x), "monotonic reads");
});

string? tool = args.Length > 1 ? args[1] : null;
if (tool == null)
{
    Console.WriteLine($"{checks} checks, {failures} failed (remote tests skipped: no sql tool given)");
    return failures == 0 ? 0 : 1;
}

int FreePort()
{
    var l = new TcpListener(IPAddress.Loopback, 0);
    l.Start();
    int port = ((IPEndPoint)l.LocalEndpoint).Port;
    l.Stop();
    return port;
}

Process Serve(string db, int port, string unixPath, params string[] more)
{
    var psi = new ProcessStartInfo(tool) { RedirectStandardOutput = true, UseShellExecute = false };
    foreach (var a in new[] { db, "--listen", port.ToString(), "--unix", unixPath }.Concat(more)) psi.ArgumentList.Add(a);
    var p = Process.Start(psi)!;
    var first = p.StandardOutput.ReadLine();
    if (first == null) throw new Exception("the server exited");
    return p;
}

int port = FreePort();
// Unix socket paths are limited to 107 bytes, which a build directory can exceed: the sockets live in /tmp.
var sock = Path.Combine(Path.GetTempPath(), $"sqlcs{Environment.ProcessId}.sock");
using var server = Serve(Path.Combine(work, "served.db"), port, sock);
try
{
    Test("remote database over WebSocket", () =>
    {
        using var db = Database.Connect($"ws://127.0.0.1:{port}/api/v1/ws/sql");
        Check(db.IsRemote, "remote");
        Common(db, "ws");
    });

    Test("remote database over a Unix socket", () =>
    {
        using var db = Database.Connect($"unix://{sock}");
        Check(db.IsRemote, "remote");
        Check(db.Tables().Contains("people"), "sees what the other connection made");
        using var r = db.Exec("SELECT count(*) FROM people");
        Check((long)r[0][0]! >= 6, "count over the socket");
        Check((long)db.Scalar("SELECT 40 + 2")! == 42, "scalar");
        Throws(ErrorCode.NoSuchTable, () => db.Exec("SELECT * FROM absent"), "error over the socket");
    });

    Test("remote read-only connections and credentials", () =>
    {
        using var ro = Database.Connect($"ws://127.0.0.1:{port}/api/v1/ws/sql", new RemoteOptions { ReadOnly = true });
        Check((long)ro.Scalar("SELECT count(*) FROM people")! >= 6, "reads");
        Throws(ErrorCode.ReadOnly, () => ro.Run("DELETE FROM people"), "read-only connection");
        Throws(ErrorCode.Connection, () => Database.Connect("ws://127.0.0.1:1/nowhere", new RemoteOptions { ConnectTimeout = TimeSpan.FromSeconds(2) }), "unreachable server");
    });

    int guardedPort = FreePort();
    var guardedSock = Path.Combine(Path.GetTempPath(), $"sqlcg{Environment.ProcessId}.sock");
    using var guarded = Serve(Path.Combine(work, "guarded.db"), guardedPort, guardedSock, "--token", "sesame");
    try
    {
        Test("a server that wants a token", () =>
        {
            var url = $"ws://127.0.0.1:{guardedPort}/api/v1/ws/sql";
            Throws(ErrorCode.PermissionDenied, () => Database.Connect(url), "no token");
            Throws(ErrorCode.PermissionDenied, () => Database.Connect(url, new RemoteOptions { Token = "wrong" }), "wrong token");
            using var db = Database.Connect(url, new RemoteOptions { Token = "sesame" });
            Check((long)db.Scalar("SELECT 1")! == 1, "right token");
        });
    }
    finally { guarded.Kill(); File.Delete(guardedSock); }
}
finally { server.Kill(); File.Delete(sock); }

Console.WriteLine($"{checks} checks, {failures} failed");
return failures == 0 ? 0 : 1;
