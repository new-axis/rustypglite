using System.Runtime.InteropServices;

namespace RustyPGlite;

/// <summary>
/// An embedded PostgreSQL server. Starts a real Postgres process listening
/// on a unix socket. Use the ConnectionString with Npgsql/EF Core.
///
/// Usage:
///   using var pg = EmbeddedPg.Start();
///   await using var conn = new NpgsqlConnection(pg.ConnectionString);
///   // or: options.UseNpgsql(pg.ConnectionString);
/// </summary>
public sealed class EmbeddedPg : IDisposable
{
    private IntPtr _handle;
    private bool _disposed;

    private EmbeddedPg(IntPtr handle)
    {
        _handle = handle;
    }

    /// <summary>
    /// Start an embedded PostgreSQL server.
    /// Runs initdb + starts postgres on a unix socket with a random port.
    /// Tuned for testing speed (fsync=off, synchronous_commit=off).
    /// </summary>
    public static EmbeddedPg Start()
    {
        var handle = NativeMethods.Start();
        if (handle == IntPtr.Zero)
            throw new PGliteException("Failed to start embedded PostgreSQL server");
        return new EmbeddedPg(handle);
    }

    /// <summary>
    /// Npgsql-compatible connection string.
    /// Use directly with NpgsqlConnection or EF Core's UseNpgsql().
    /// </summary>
    public string ConnectionString
    {
        get
        {
            ThrowIfDisposed();
            var ptr = NativeMethods.ConnectionString(_handle);
            return Marshal.PtrToStringUTF8(ptr) ?? "";
        }
    }

    /// <summary>Unix socket directory path.</summary>
    public string SocketDir
    {
        get
        {
            ThrowIfDisposed();
            var ptr = NativeMethods.SocketDir(_handle);
            return Marshal.PtrToStringUTF8(ptr) ?? "";
        }
    }

    /// <summary>Port number the server is listening on.</summary>
    public int Port
    {
        get
        {
            ThrowIfDisposed();
            return NativeMethods.Port(_handle);
        }
    }

    /// <summary>Data directory path.</summary>
    public string DataDir
    {
        get
        {
            ThrowIfDisposed();
            var ptr = NativeMethods.DataDir(_handle);
            return Marshal.PtrToStringUTF8(ptr) ?? "";
        }
    }

    /// <summary>Create a new database on this server.</summary>
    public void CreateDatabase(string name)
    {
        ThrowIfDisposed();
        var rc = NativeMethods.CreateDatabase(_handle, name);
        if (rc != 0)
            throw new PGliteException($"Failed to create database '{name}'");
    }

    /// <summary>Execute SQL via psql (useful for DDL/migrations before handing off to Npgsql).</summary>
    public void ExecuteSql(string sql, string? dbName = null)
    {
        ThrowIfDisposed();
        var rc = NativeMethods.ExecSql(_handle, dbName, sql);
        if (rc != 0)
            throw new PGliteException($"SQL execution failed: {sql[..Math.Min(sql.Length, 100)]}");
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            _disposed = true;
            if (_handle != IntPtr.Zero)
            {
                NativeMethods.Stop(_handle);
                _handle = IntPtr.Zero;
            }
        }
    }
}
