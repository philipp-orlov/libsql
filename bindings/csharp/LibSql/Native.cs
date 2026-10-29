// Copyright (c) 2026 Philipp Orlov

using System.Runtime.InteropServices;

namespace LibSql;

/// <summary>The C interface (include/sql/sql_c.h). Status: &gt;= 0 success, &lt; 0 is -(ErrorCode).</summary>
internal static unsafe partial class Native
{
    const string Lib = "sql_c";

    static Native()
    {
        // SQL_LIBRARY names the shared library to load when it is not next to the assembly or on the loader path.
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, (name, assembly, path) =>
        {
            if (name != Lib) return IntPtr.Zero;
            var explicitPath = Environment.GetEnvironmentVariable("SQL_LIBRARY");
            if (!string.IsNullOrEmpty(explicitPath) && NativeLibrary.TryLoad(explicitPath, out var h)) return h;
            return NativeLibrary.TryLoad(name, assembly, path, out h) ? h : IntPtr.Zero;
        });
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Value
    {
        public int Type, Reserved;
        public long I;
        public double D;
        public byte* Data;
        public nuint Size;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Options
    {
        public ulong MaxSize, BufferCache;
        public uint BusyTimeoutMs, SyncIntervalMs;
        public int Durability, MemoryProfile;
        public byte ReadOnly, NoCreate, R0, R1, R2, R3, R4, R5;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RemoteOptions
    {
        public byte* Token, Authorization, CaFile, ClientName;
        public uint ConnectTimeoutMs, RequestTimeoutMs, PingIntervalMs;
        public byte ReadOnly, InsecureSkipVerify, NoReconnect, R0, R1, R2, R3, R4;
    }

    [LibraryImport(Lib)] public static partial IntPtr sql_version();
    [LibraryImport(Lib)] public static partial IntPtr sql_last_error();
    [LibraryImport(Lib)] public static partial int sql_last_constraint();
    [LibraryImport(Lib)] public static partial int sql_complete(byte* text, nuint len);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial int sql_open(string path, in Options options, out IntPtr db);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] public static partial int sql_connect(string url, in RemoteOptions options, out IntPtr db);
    [LibraryImport(Lib)] public static partial void sql_close(IntPtr db);
    [LibraryImport(Lib)] public static partial int sql_is_remote(IntPtr db);
    [LibraryImport(Lib)] public static partial int sql_open_reader(IntPtr db, out IntPtr reader);
    [LibraryImport(Lib)] public static partial int sql_exec(IntPtr db, byte* text, nuint len, Value* args, nuint n, out IntPtr result);
    [LibraryImport(Lib)] public static partial int sql_changes(IntPtr db, out long n);
    [LibraryImport(Lib)] public static partial int sql_total_changes(IntPtr db, out long n);
    [LibraryImport(Lib)] public static partial int sql_last_insert_rowid(IntPtr db, out long n);
    [LibraryImport(Lib)] public static partial int sql_in_transaction(IntPtr db);
    [LibraryImport(Lib)] public static partial void sql_interrupt(IntPtr db);

    [LibraryImport(Lib)] public static partial int sql_begin(IntPtr db, int readOnly, out IntPtr txn);
    [LibraryImport(Lib)] public static partial int sql_txn_exec(IntPtr txn, byte* text, nuint len, Value* args, nuint n, out IntPtr result);
    [LibraryImport(Lib)] public static partial int sql_commit(IntPtr txn);
    [LibraryImport(Lib)] public static partial void sql_rollback(IntPtr txn);

    [LibraryImport(Lib)] public static partial int sql_prepare(IntPtr db, byte* text, nuint len, out IntPtr stmt);
    [LibraryImport(Lib)] public static partial void sql_stmt_free(IntPtr stmt);
    [LibraryImport(Lib)] public static partial int sql_stmt_parameters(IntPtr stmt);
    [LibraryImport(Lib)] public static partial int sql_stmt_parameter_index(IntPtr stmt, byte* name, nuint len);
    [LibraryImport(Lib)] public static partial int sql_stmt_parameter_name(IntPtr stmt, int index, out byte* name, out nuint len);
    [LibraryImport(Lib)] public static partial int sql_stmt_writes(IntPtr stmt);
    [LibraryImport(Lib)] public static partial int sql_stmt_bind(IntPtr stmt, int index, in Value value);
    [LibraryImport(Lib)] public static partial int sql_stmt_clear(IntPtr stmt);
    [LibraryImport(Lib)] public static partial int sql_stmt_exec(IntPtr stmt, IntPtr txn, out IntPtr result);

    [LibraryImport(Lib)] public static partial void sql_result_free(IntPtr result);
    [LibraryImport(Lib)] public static partial nuint sql_result_columns(IntPtr result);
    [LibraryImport(Lib)] public static partial int sql_result_column_name(IntPtr result, nuint column, out byte* name, out nuint len);
    [LibraryImport(Lib)] public static partial nuint sql_result_rows(IntPtr result);
    [LibraryImport(Lib)] public static partial int sql_result_cell(IntPtr result, nuint row, nuint column, out Value value);
    [LibraryImport(Lib)] public static partial int sql_result_row(IntPtr result, nuint row, Value* values, nuint capacity);
    [LibraryImport(Lib)] public static partial long sql_result_changes(IntPtr result);
    [LibraryImport(Lib)] public static partial long sql_result_last_insert_id(IntPtr result);

    [LibraryImport(Lib)] public static partial int sql_tables(IntPtr db, out byte* names, out nuint len, out nuint count);
    [LibraryImport(Lib)] public static partial int sql_table_open(IntPtr db, byte* name, nuint len, out IntPtr table);
    [LibraryImport(Lib)] public static partial void sql_table_free(IntPtr table);
    [LibraryImport(Lib)] public static partial int sql_table_columns(IntPtr table);
    [LibraryImport(Lib)] public static partial int sql_table_column(IntPtr table, int index, out byte* name, out nuint nameLen, out byte* declared, out nuint declaredLen, out int flags);
    [LibraryImport(Lib)] public static partial int sql_table_sql(IntPtr table, out byte* text, out nuint len);

    public static string LastError() => Marshal.PtrToStringUTF8(sql_last_error()) ?? "";

    public static int Check(int status)
    {
        if (status < 0) throw new SqlException((ErrorCode)(-status), (Constraint)sql_last_constraint(), LastError());
        return status;
    }
}
