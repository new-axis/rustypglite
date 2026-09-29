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
///
/// A server never outlives the process that started it: a watchdog stops it
/// and removes its auto data directory when this process ends, however it
/// ends (even kill -9), and every start sweeps away servers whose owners are
/// provably dead. The exception is a server started with
/// <see cref="EmbeddedPgOptions.Durable"/>, which runs until it is stopped
/// explicitly (<see cref="Stop"/> or <see cref="StopDir"/>).
/// </summary>
public sealed class EmbeddedPg : IDisposable
{
    private IntPtr _handle;
    private bool _disposed;

    private EmbeddedPg(IntPtr handle, bool durable)
    {
        _handle = handle;
        IsDurable = durable;
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
        return new EmbeddedPg(handle, durable: false);
    }

    /// <summary>
    /// Start an embedded PostgreSQL server with options (see <see cref="EmbeddedPgOptions"/>).
    /// </summary>
    public static EmbeddedPg Start(EmbeddedPgOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        if (options.Port is < 0 or > 65535)
            throw new ArgumentOutOfRangeException(nameof(options), "Port must be between 0 (automatic) and 65535");
        var handle = NativeMethods.StartWith(
            options.DataDir,
            options.DbName,
            options.Port ?? 0,
            options.KeepData ? 1 : 0,
            options.Durable ? 1 : 0,
            options.TempRoot);
        if (handle == IntPtr.Zero)
            throw new PGliteException("Failed to start embedded PostgreSQL server");
        return new EmbeddedPg(handle, options.Durable);
    }

    /// <summary>
    /// Stop the server running in <paramref name="dataDir"/>, from any process —
    /// the explicit stop, and the way to end a durable server. Removes the
    /// directory only when its owner.json says rustypglite created it; never a
    /// data directory you supplied. Succeeds if no server is left running there.
    /// </summary>
    /// <exception cref="PGliteException">It is not a data directory of this user's, or the server would not stop.</exception>
    public static void StopDir(string dataDir)
    {
        ArgumentNullException.ThrowIfNull(dataDir);
        var rc = NativeMethods.StopDir(dataDir);
        if (rc != 0)
            throw new PGliteException(rc == -4
                ? $"The server in '{dataDir}' would not stop"
                : $"'{dataDir}' is not a PostgreSQL data directory of this user's (code {rc})");
    }

    /// <summary>
    /// Reclaim servers whose owner is provably dead, under <paramref name="tempRoot"/>
    /// (null = $RUSTYPGLITE_TMPDIR, else /tmp). Touches only rpgl_* directories
    /// whose owner.json shows the owner (PID + start time) and its watchdog are
    /// both gone — never a live owner's, a durable one, or one without owner.json.
    /// Every start already does this; call it to report, or to sweep without starting.
    /// </summary>
    /// <returns>What was found, or null if another process is sweeping right now.</returns>
    /// <exception cref="PGliteException">The root is missing or cannot be written.</exception>
    public static SweepReport? Sweep(string? tempRoot = null)
    {
        var rc = NativeMethods.Sweep(tempRoot, out var r);
        if (rc == -2)
            return null;
        if (rc != 0)
            throw new PGliteException($"Sweep of '{tempRoot ?? "(default temp root)"}' failed (code {rc})");
        return new SweepReport(r.Examined, r.Reclaimed, r.Live, r.Durable, r.Legacy, r.Skipped, r.Failed);
    }

    /// <summary>Whether this server was started durable (see <see cref="EmbeddedPgOptions.Durable"/>).</summary>
    public bool IsDurable { get; }

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

    /// <summary>
    /// Stop the server and clean up — a durable one too: this is the explicit
    /// stop. The instance is disposed afterwards.
    /// </summary>
    public void Stop() => Release(NativeMethods.StopServer);

    /// <summary>
    /// Let go of the server without stopping it. A durable server keeps running
    /// after this process exits; a non-durable one is still stopped when this
    /// process exits. The instance is disposed afterwards.
    /// </summary>
    public void Detach() => Release(NativeMethods.Detach);

    /// <summary>
    /// Stops a non-durable server and removes its auto data directory; for a
    /// durable server, only lets go of it (as <see cref="Detach"/>).
    /// </summary>
    public void Dispose() => Release(NativeMethods.Stop);

    private void Release(Action<IntPtr> release)
    {
        if (!_disposed)
        {
            _disposed = true;
            if (_handle != IntPtr.Zero)
            {
                release(_handle);
                _handle = IntPtr.Zero;
            }
        }
    }
}
