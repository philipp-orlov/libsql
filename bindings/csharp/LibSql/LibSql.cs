// Copyright (c) 2026 Philipp Orlov

using System.Runtime.InteropServices;
using System.Text;

namespace LibSql;

public enum ErrorCode
{
    Ok = 0, SyntaxError, Unsupported, InvalidArgument, NoSuchTable, TableExists, NoSuchColumn, AmbiguousColumn, NoSuchIndex, IndexExists,
    TypeMismatch, ConstraintViolation, BadTransaction, ReadOnly, Internal, Runtime, Busy, Interrupted, Connection, Protocol, PermissionDenied,
}

public enum Constraint { None = 0, PrimaryKey, Unique, NotNull, Check, ForeignKey, Trigger, Datatype }

public sealed class SqlException : Exception
{
    public ErrorCode Code { get; }
    public Constraint Constraint { get; }
    public SqlException(ErrorCode code, Constraint constraint, string message) : base(message) { Code = code; Constraint = constraint; }
}

public enum ValueType { Null = 0, Integer = 1, Real = 2, Datetime = 3, Text = 4, Blob = 5 }

public enum Durability { Default = 0, Safe = 1, NoMetaSync = 2, None = 3, Lazy = 4 }
public enum MemoryProfile { Balanced = 0, Compact = 1, Throughput = 2 }

public sealed class OpenOptions
{
    public ulong MaxSize, BufferCache;
    public TimeSpan BusyTimeout, SyncInterval;
    public Durability Durability;
    public MemoryProfile MemoryProfile;
    public bool ReadOnly;
    public bool CreateIfMissing = true;
}

public sealed class RemoteOptions
{
    public string? Token, Authorization, CaFile, ClientName;
    public TimeSpan ConnectTimeout, RequestTimeout, PingInterval;
    public bool ReadOnly, InsecureSkipVerify, Reconnect = true;
}

public readonly record struct ColumnInfo(string Name, string DeclaredType, bool PrimaryKey, bool NotNull, bool Unique, bool HasDefault);
public sealed record TableInfo(string Name, IReadOnlyList<ColumnInfo> Columns, string Sql);

/// <summary>
/// The arguments of a call, copied into native memory for its duration. Accepted: null, bool, the integer types, float, double,
/// DateTime and DateTimeOffset (stored as microseconds since the epoch), string, byte[] and ReadOnlyMemory&lt;byte&gt;.
/// </summary>
internal unsafe struct Args : IDisposable
{
    public Native.Value* Values;
    public int Count;
    byte* _bytes;

    public static Args From(ReadOnlySpan<object?> items)
    {
        var a = new Args { Count = items.Length };
        if (items.Length == 0) return a;
        long total = 0;
        foreach (var o in items) total += o switch { string s => Encoding.UTF8.GetMaxByteCount(s.Length), byte[] b => b.Length, ReadOnlyMemory<byte> m => m.Length, _ => 0 };
        a.Values = (Native.Value*)NativeMemory.AllocZeroed((nuint)items.Length, (nuint)sizeof(Native.Value));
        a._bytes = total > 0 ? (byte*)NativeMemory.Alloc((nuint)total) : null;
        byte* next = a._bytes;
        for (int i = 0; i < items.Length; i++) a.Values[i] = Convert(items[i], ref next);
        return a;
    }

    public static Native.Value Single(object? o, out byte* owned)
    {
        byte* next = null;
        if (o is string s) next = (byte*)NativeMemory.Alloc((nuint)Math.Max(1, Encoding.UTF8.GetMaxByteCount(s.Length)));
        else if (o is byte[] b) next = (byte*)NativeMemory.Alloc((nuint)Math.Max(1, b.Length));
        else if (o is ReadOnlyMemory<byte> m) next = (byte*)NativeMemory.Alloc((nuint)Math.Max(1, m.Length));
        owned = next;
        return Convert(o, ref next);
    }

    static Native.Value Convert(object? o, ref byte* next)
    {
        var v = new Native.Value();
        switch (o)
        {
            case null: case DBNull: v.Type = (int)ValueType.Null; break;
            case bool x: v.Type = (int)ValueType.Integer; v.I = x ? 1 : 0; break;
            case sbyte x: v.Type = (int)ValueType.Integer; v.I = x; break;
            case byte x: v.Type = (int)ValueType.Integer; v.I = x; break;
            case short x: v.Type = (int)ValueType.Integer; v.I = x; break;
            case ushort x: v.Type = (int)ValueType.Integer; v.I = x; break;
            case int x: v.Type = (int)ValueType.Integer; v.I = x; break;
            case uint x: v.Type = (int)ValueType.Integer; v.I = x; break;
            case long x: v.Type = (int)ValueType.Integer; v.I = x; break;
            case ulong x when x <= long.MaxValue: v.Type = (int)ValueType.Integer; v.I = (long)x; break;
            case float x: v.Type = (int)ValueType.Real; v.D = x; break;
            case double x: v.Type = (int)ValueType.Real; v.D = x; break;
            case decimal x: v.Type = (int)ValueType.Real; v.D = (double)x; break;
            case DateTime x: v.Type = (int)ValueType.Datetime; v.I = (x.ToUniversalTime() - DateTime.UnixEpoch).Ticks / 10; break;
            case DateTimeOffset x: v.Type = (int)ValueType.Datetime; v.I = (x.UtcDateTime - DateTime.UnixEpoch).Ticks / 10; break;
            case string x:
            {
                v.Type = (int)ValueType.Text;
                int n = Encoding.UTF8.GetBytes(x, new Span<byte>(next, Encoding.UTF8.GetMaxByteCount(x.Length)));
                v.Data = next; v.Size = (nuint)n; next += n;
                break;
            }
            case byte[] x: Blob(x, ref next, ref v); break;
            case ReadOnlyMemory<byte> x: Blob(x.Span, ref next, ref v); break;
            default: throw new ArgumentException($"cannot bind a {o.GetType()}");
        }
        return v;
    }

    static void Blob(ReadOnlySpan<byte> x, ref byte* next, ref Native.Value v)
    {
        v.Type = (int)ValueType.Blob;
        x.CopyTo(new Span<byte>(next, x.Length));
        v.Data = next; v.Size = (nuint)x.Length; next += x.Length;
    }

    public void Dispose()
    {
        NativeMemory.Free(Values);
        NativeMemory.Free(_bytes);
        Values = null; _bytes = null;
    }
}

/// <summary>A database: a local file (<see cref="Open"/>) or a server (<see cref="Connect"/>), used the same way.</summary>
public sealed unsafe class Database : IDisposable
{
    IntPtr _h;

    Database(IntPtr h) => _h = h;
    internal IntPtr Handle => _h != IntPtr.Zero ? _h : throw new ObjectDisposedException(nameof(Database));

    public static string Version => Marshal.PtrToStringUTF8(Native.sql_version()) ?? "";

    /// <summary>True when the text is one or more whole statements (a REPL uses it to know when to run).</summary>
    public static bool IsComplete(string text)
    {
        var b = Encoding.UTF8.GetBytes(text);
        fixed (byte* p = b) return Native.sql_complete(p, (nuint)b.Length) != 0;
    }

    public static Database Open(string path, OpenOptions? options = null)
    {
        options ??= new OpenOptions();
        var o = new Native.Options
        {
            MaxSize = options.MaxSize, BufferCache = options.BufferCache, BusyTimeoutMs = (uint)options.BusyTimeout.TotalMilliseconds,
            SyncIntervalMs = (uint)options.SyncInterval.TotalMilliseconds, Durability = (int)options.Durability,
            MemoryProfile = (int)options.MemoryProfile, ReadOnly = (byte)(options.ReadOnly ? 1 : 0), NoCreate = (byte)(options.CreateIfMissing ? 0 : 1),
        };
        Native.Check(Native.sql_open(path, o, out var h));
        return new Database(h);
    }

    /// <summary>Connects to a server: ws://host:port/path, wss://… or unix:///path/to/socket.</summary>
    public static Database Connect(string url, RemoteOptions? options = null)
    {
        options ??= new RemoteOptions();
        IntPtr token = Utf8(options.Token), auth = Utf8(options.Authorization), ca = Utf8(options.CaFile), name = Utf8(options.ClientName);
        try
        {
            var o = new Native.RemoteOptions
            {
                Token = (byte*)token, Authorization = (byte*)auth, CaFile = (byte*)ca, ClientName = (byte*)name,
                ConnectTimeoutMs = (uint)options.ConnectTimeout.TotalMilliseconds, RequestTimeoutMs = (uint)options.RequestTimeout.TotalMilliseconds,
                PingIntervalMs = (uint)options.PingInterval.TotalMilliseconds, ReadOnly = (byte)(options.ReadOnly ? 1 : 0),
                InsecureSkipVerify = (byte)(options.InsecureSkipVerify ? 1 : 0), NoReconnect = (byte)(options.Reconnect ? 0 : 1),
            };
            Native.Check(Native.sql_connect(url, o, out var h));
            return new Database(h);
        }
        finally
        {
            foreach (var p in new[] { token, auth, ca, name }) Marshal.FreeCoTaskMem(p);
        }
    }

    static IntPtr Utf8(string? s) => s == null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(s);

    public bool IsRemote => Native.sql_is_remote(Handle) != 0;

    /// <summary>Another handle on the same store for a reading thread.</summary>
    public Database OpenReader()
    {
        Native.Check(Native.sql_open_reader(Handle, out var h));
        return new Database(h);
    }

    /// <summary>Runs the statements; the last one's rows are the result. Dispose it when done with the rows.</summary>
    public Result Exec(string sql, params object?[] args) => Exec(sql, (ReadOnlySpan<object?>)args);

    public Result Exec(string sql, ReadOnlySpan<object?> args)
    {
        var text = Encoding.UTF8.GetBytes(sql);
        using var a = Args.From(args);
        fixed (byte* t = text)
        {
            Native.Check(Native.sql_exec(Handle, t, (nuint)text.Length, a.Values, (nuint)a.Count, out var r));
            return new Result(r);
        }
    }

    /// <summary>Runs a statement for its effect and returns the number of rows it changed.</summary>
    public long Run(string sql, params object?[] args)
    {
        using var r = Exec(sql, args);
        return r.Changes;
    }

    /// <summary>The first column of the first row, or null.</summary>
    public object? Scalar(string sql, params object?[] args)
    {
        using var r = Exec(sql, args);
        return r.RowCount == 0 ? null : r[0][0];
    }

    public Statement Prepare(string sql)
    {
        var text = Encoding.UTF8.GetBytes(sql);
        fixed (byte* t = text)
        {
            Native.Check(Native.sql_prepare(Handle, t, (nuint)text.Length, out var s));
            return new Statement(s);
        }
    }

    /// <summary>Starts a transaction; dispose it without committing to roll it back.</summary>
    public Transaction Begin(bool readOnly = false)
    {
        Native.Check(Native.sql_begin(Handle, readOnly ? 1 : 0, out var t));
        return new Transaction(t);
    }

    public long Changes { get { Native.Check(Native.sql_changes(Handle, out var n)); return n; } }
    public long TotalChanges { get { Native.Check(Native.sql_total_changes(Handle, out var n)); return n; } }
    public long LastInsertRowid { get { Native.Check(Native.sql_last_insert_rowid(Handle, out var n)); return n; } }
    public bool InTransaction => Native.sql_in_transaction(Handle) != 0;

    /// <summary>Stops the statement running on another thread; it fails with <see cref="ErrorCode.Interrupted"/>.</summary>
    public void Interrupt() => Native.sql_interrupt(Handle);

    public IReadOnlyList<string> Tables()
    {
        Native.Check(Native.sql_tables(Handle, out var names, out var len, out _));
        var all = Encoding.UTF8.GetString(names, (int)len);
        return all.Length == 0 ? Array.Empty<string>() : all.TrimEnd('\0').Split('\0');
    }

    public TableInfo Table(string name)
    {
        var n = Encoding.UTF8.GetBytes(name);
        fixed (byte* p = n)
        {
            Native.Check(Native.sql_table_open(Handle, p, (nuint)n.Length, out var t));
            try
            {
                int count = Native.sql_table_columns(t);
                var cols = new List<ColumnInfo>(count);
                for (int i = 0; i < count; i++)
                {
                    Native.Check(Native.sql_table_column(t, i, out var cn, out var cl, out var dn, out var dl, out var flags));
                    cols.Add(new ColumnInfo(Encoding.UTF8.GetString(cn, (int)cl), Encoding.UTF8.GetString(dn, (int)dl),
                                            (flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0, (flags & 8) != 0));
                }
                Native.sql_table_sql(t, out var sn, out var sl);
                return new TableInfo(name, cols, Encoding.UTF8.GetString(sn, (int)sl));
            }
            finally { Native.sql_table_free(t); }
        }
    }

    public void Dispose()
    {
        var h = _h;
        _h = IntPtr.Zero;
        if (h != IntPtr.Zero) Native.sql_close(h);
    }
}

/// <summary>A transaction. Dispose without <see cref="Commit"/> rolls it back.</summary>
public sealed unsafe class Transaction : IDisposable
{
    IntPtr _h;

    internal Transaction(IntPtr h) => _h = h;
    internal IntPtr Handle => _h != IntPtr.Zero ? _h : throw new ObjectDisposedException(nameof(Transaction));

    public Result Exec(string sql, params object?[] args)
    {
        var text = Encoding.UTF8.GetBytes(sql);
        using var a = Args.From(args);
        fixed (byte* t = text)
        {
            Native.Check(Native.sql_txn_exec(Handle, t, (nuint)text.Length, a.Values, (nuint)a.Count, out var r));
            return new Result(r);
        }
    }

    public long Run(string sql, params object?[] args)
    {
        using var r = Exec(sql, args);
        return r.Changes;
    }

    public void Commit()
    {
        var h = Handle;
        _h = IntPtr.Zero;
        Native.Check(Native.sql_commit(h));
    }

    public void Rollback() => Dispose();

    public void Dispose()
    {
        var h = _h;
        _h = IntPtr.Zero;
        if (h != IntPtr.Zero) Native.sql_rollback(h);
    }
}

/// <summary>A statement parsed once and run many times. Parameter indexes are 1-based.</summary>
public sealed unsafe class Statement : IDisposable
{
    IntPtr _h;

    internal Statement(IntPtr h) => _h = h;
    IntPtr Handle => _h != IntPtr.Zero ? _h : throw new ObjectDisposedException(nameof(Statement));

    public int Parameters => Native.Check(Native.sql_stmt_parameters(Handle));
    public bool Writes => Native.Check(Native.sql_stmt_writes(Handle)) != 0;

    public int ParameterIndex(string name)
    {
        var b = Encoding.UTF8.GetBytes(name);
        fixed (byte* p = b) return Native.Check(Native.sql_stmt_parameter_index(Handle, p, (nuint)b.Length));
    }

    public string ParameterName(int index)
    {
        Native.Check(Native.sql_stmt_parameter_name(Handle, index, out var n, out var l));
        return Encoding.UTF8.GetString(n, (int)l);
    }

    public Statement Bind(int index, object? value)
    {
        var v = Args.Single(value, out var owned);
        try { Native.Check(Native.sql_stmt_bind(Handle, index, v)); }
        finally { NativeMemory.Free(owned); }
        return this;
    }

    public Statement Bind(string name, object? value)
    {
        int i = ParameterIndex(name);
        if (i == 0) throw new ArgumentException($"no parameter {name}");
        return Bind(i, value);
    }

    public Statement Bind(params object?[] values)
    {
        for (int i = 0; i < values.Length; i++) Bind(i + 1, values[i]);
        return this;
    }

    public Statement Clear()
    {
        Native.Check(Native.sql_stmt_clear(Handle));
        return this;
    }

    public Result Exec(Transaction? txn = null)
    {
        Native.Check(Native.sql_stmt_exec(Handle, txn?.Handle ?? IntPtr.Zero, out var r));
        return new Result(r);
    }

    public Result Exec(params object?[] values) => Bind(values).Exec();

    public void Dispose()
    {
        var h = _h;
        _h = IntPtr.Zero;
        if (h != IntPtr.Zero) Native.sql_stmt_free(h);
    }
}

/// <summary>The rows a statement returned. Text and blob cells point into it: valid until it is disposed.</summary>
public sealed unsafe class Result : IDisposable
{
    IntPtr _h;
    string[]? _columns;

    internal Result(IntPtr h) => _h = h;
    IntPtr Handle => _h != IntPtr.Zero ? _h : throw new ObjectDisposedException(nameof(Result));

    public int RowCount => (int)Native.sql_result_rows(Handle);
    public int ColumnCount => (int)Native.sql_result_columns(Handle);
    public long Changes => Native.sql_result_changes(Handle);
    public long LastInsertId => Native.sql_result_last_insert_id(Handle);

    public IReadOnlyList<string> Columns
    {
        get
        {
            if (_columns != null) return _columns;
            var cols = new string[ColumnCount];
            for (int i = 0; i < cols.Length; i++)
            {
                Native.Check(Native.sql_result_column_name(Handle, (nuint)i, out var n, out var l));
                cols[i] = Encoding.UTF8.GetString(n, (int)l);
            }
            return _columns = cols;
        }
    }

    public int ColumnIndex(string name)
    {
        var cols = Columns;
        for (int i = 0; i < cols.Count; i++) if (cols[i] == name) return i;
        throw new KeyNotFoundException($"no column {name}");
    }

    public Row this[int row] => row >= 0 && row < RowCount ? new Row(this, row) : throw new ArgumentOutOfRangeException(nameof(row));

    public IEnumerable<Row> Rows
    {
        get { int n = RowCount; for (int i = 0; i < n; i++) yield return new Row(this, i); }
    }

    internal Cell Cell(int row, int column)
    {
        Native.Check(Native.sql_result_cell(Handle, (nuint)row, (nuint)column, out var v));
        return new Cell(v);
    }

    public void Dispose()
    {
        var h = _h;
        _h = IntPtr.Zero;
        if (h != IntPtr.Zero) Native.sql_result_free(h);
    }
}

/// <summary>One row of a <see cref="Result"/>.</summary>
public readonly struct Row
{
    readonly Result _r;
    readonly int _row;

    internal Row(Result r, int row) { _r = r; _row = row; }

    public int Count => _r.ColumnCount;
    public Cell Cell(int column) => _r.Cell(_row, column);
    public Cell Cell(string column) => _r.Cell(_row, _r.ColumnIndex(column));

    /// <summary>The cell as a .NET object: null, long, double, DateTime, string or byte[].</summary>
    public object? this[int column] => Cell(column).ToObject();
    public object? this[string column] => Cell(column).ToObject();

    public long GetInt64(int column) => Cell(column).Integer;
    public double GetDouble(int column) => Cell(column).Real;
    public string GetString(int column) => Cell(column).Text;
    public bool IsNull(int column) => Cell(column).Type == ValueType.Null;
}

/// <summary>A value out of a result, looked at in place. Text and Blob views are valid until the result is disposed.</summary>
public readonly unsafe struct Cell
{
    readonly Native.Value _v;

    internal Cell(Native.Value v) => _v = v;

    public ValueType Type => (ValueType)_v.Type;
    public bool IsNull => _v.Type == 0;

    public long Integer => _v.Type switch
    {
        1 or 3 => _v.I,
        2 => (long)_v.D,
        0 => 0,
        _ => long.TryParse(Text, out var n) ? n : throw new InvalidCastException("not an integer"),
    };

    public double Real => _v.Type switch
    {
        2 => _v.D,
        1 or 3 => _v.I,
        0 => 0,
        _ => double.TryParse(Text, System.Globalization.CultureInfo.InvariantCulture, out var d) ? d : throw new InvalidCastException("not a number"),
    };

    public DateTime Datetime => _v.Type == 3 ? DateTime.UnixEpoch.AddTicks(_v.I * 10) : throw new InvalidCastException("not a datetime");

    /// <summary>The bytes of a text or blob cell, no copy.</summary>
    public ReadOnlySpan<byte> Bytes => _v.Type is 4 or 5 ? new ReadOnlySpan<byte>(_v.Data, (int)_v.Size) : default;

    public string Text => _v.Type switch
    {
        4 => Encoding.UTF8.GetString(_v.Data, (int)_v.Size),
        0 => "",
        1 => _v.I.ToString(),
        2 => _v.D.ToString("R", System.Globalization.CultureInfo.InvariantCulture),
        3 => Datetime.ToString("o"),
        _ => Convert.ToHexString(Bytes),
    };

    public object? ToObject() => _v.Type switch
    {
        0 => null,
        1 => _v.I,
        2 => _v.D,
        3 => Datetime,
        4 => Text,
        _ => Bytes.ToArray(),
    };

    public override string ToString() => Text;
}
