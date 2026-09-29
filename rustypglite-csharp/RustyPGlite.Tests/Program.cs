using RustyPGlite;

namespace RustyPGlite.Tests;

/// <summary>
/// Entry point for the owner processes the lifecycle tests kill. Under
/// `dotnet test` this is never called (the test host loads the assembly);
/// `dotnet RustyPGlite.Tests.dll hold &lt;root&gt; [durable]` starts a server
/// under &lt;root&gt;, prints RPGL_DIR=&lt;data dir&gt; and waits to be killed.
/// </summary>
public static class Program
{
    public static int Main(string[] args)
    {
        if (args.Length < 2 || args[0] != "hold")
            return 0;

        var pg = EmbeddedPg.Start(new EmbeddedPgOptions
        {
            TempRoot = args[1],
            Durable = args.Length > 2 && args[2] == "durable",
        });
        Console.WriteLine($"RPGL_DIR={pg.DataDir}");
        Console.Out.Flush();
        Thread.Sleep(Timeout.Infinite); // until killed
        GC.KeepAlive(pg);
        return 0;
    }
}
