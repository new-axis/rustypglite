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
}
