using System.Runtime.InteropServices;

namespace RustyPGlite;

/// <summary>
/// P/Invoke declarations for the native rustypglite library.
/// </summary>
internal static partial class NativeMethods
{
    private const string LibName = "rustypglite";

    [LibraryImport(LibName, EntryPoint = "rpglite_start")]
    internal static partial IntPtr Start();

    [LibraryImport(LibName, EntryPoint = "rpglite_stop")]
    internal static partial void Stop(IntPtr pg);

    [LibraryImport(LibName, EntryPoint = "rpglite_connection_string")]
    internal static partial IntPtr ConnectionString(IntPtr pg);

    [LibraryImport(LibName, EntryPoint = "rpglite_socket_dir")]
    internal static partial IntPtr SocketDir(IntPtr pg);

    [LibraryImport(LibName, EntryPoint = "rpglite_port")]
    internal static partial int Port(IntPtr pg);

    [LibraryImport(LibName, EntryPoint = "rpglite_data_dir")]
    internal static partial IntPtr DataDir(IntPtr pg);

    [LibraryImport(LibName, EntryPoint = "rpglite_create_database", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int CreateDatabase(IntPtr pg, string name);

    [LibraryImport(LibName, EntryPoint = "rpglite_exec_sql", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int ExecSql(IntPtr pg, string? dbName, string sql);

    [LibraryImport(LibName, EntryPoint = "rpglite_start_with", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr StartWith(string? dataDir, string? dbName, int port,
        int keepData, int durable, string? tempRoot);

    [LibraryImport(LibName, EntryPoint = "rpglite_stop_server")]
    internal static partial void StopServer(IntPtr pg);

    [LibraryImport(LibName, EntryPoint = "rpglite_detach")]
    internal static partial void Detach(IntPtr pg);

    [LibraryImport(LibName, EntryPoint = "rpglite_stop_dir", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int StopDir(string dataDir);

    [LibraryImport(LibName, EntryPoint = "rpglite_sweep", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int Sweep(string? tempRoot, out SweepResult result);

    /// <summary>Mirror of rpgl_sweep_result in pg_shim.h: seven int32s, in this order.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct SweepResult
    {
        public int Examined, Reclaimed, Live, Durable, Legacy, Skipped, Failed;
    }
}
