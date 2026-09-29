using System.Diagnostics;
using System.Text.RegularExpressions;
using Xunit;
using Xunit.Abstractions;

namespace RustyPGlite.Tests;

/// <summary>
/// No leaked servers: a server dies with its owner however the owner ends,
/// the sweep reclaims only provably dead owners' servers, and a durable
/// server is left alone until it is stopped explicitly. Mirrors
/// rustypglite/tests/lifecycle_test.rs.
///
/// Every test works in its own temp root (never the real /tmp/rpgl_*), so the
/// sweeps here cannot touch anyone else's servers. The root's parent is
/// $RUSTYPGLITE_TEST_ROOT, else /tmp — kept short, because the unix socket
/// path inside it is limited to ~100 bytes (and macOS's temp path is long).
///
/// Owner processes that get killed are this test assembly run as a program
/// (see Program.cs): a real .NET process using this binding.
/// </summary>
public class LifecycleTests
{
    private readonly ITestOutputHelper _output;

    public LifecycleTests(ITestOutputHelper output)
    {
        _output = output;
    }

    // ---- helpers ----

    /// <summary>A temp root of the test's own; stops whatever is left in it and removes it.</summary>
    private sealed class Root : IDisposable
    {
        public string Path { get; }

        public Root(string name)
        {
            var baseDir = Environment.GetEnvironmentVariable("RUSTYPGLITE_TEST_ROOT") ?? "/tmp";
            Path = System.IO.Path.Combine(baseDir, $"rpglcs-{Environment.ProcessId}-{name}");
            Directory.CreateDirectory(Path);
        }

        public void Dispose()
        {
            if (!Directory.Exists(Path))
                return;
            foreach (var dir in Directory.GetDirectories(Path))
            {
                try { EmbeddedPg.StopDir(dir); } catch (PGliteException) { }
            }
            try { Directory.Delete(Path, recursive: true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }
    }

    /// <summary>An owner process; killed on dispose if the test fails before killing it.</summary>
    private sealed class Owner : IDisposable
    {
        private readonly Process _process;
        public string Dir { get; }
        public int Pid => _process.Id;

        public Owner(Root root, bool durable)
        {
            var dll = typeof(LifecycleTests).Assembly.Location;
            // The same dotnet host that runs the tests (dotnet test sets DOTNET_HOST_PATH).
            var host = Environment.GetEnvironmentVariable("DOTNET_HOST_PATH") is { Length: > 0 } h ? h : "dotnet";
            var psi = new ProcessStartInfo(host)
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            };
            psi.ArgumentList.Add(dll);
            psi.ArgumentList.Add("hold");
            psi.ArgumentList.Add(root.Path);
            if (durable)
                psi.ArgumentList.Add("durable");
            _process = Process.Start(psi)!;
            _process.ErrorDataReceived += (_, _) => { };
            _process.BeginErrorReadLine();

            string? line;
            while ((line = _process.StandardOutput.ReadLine()) != null)
            {
                var at = line.IndexOf("RPGL_DIR=", StringComparison.Ordinal);
                if (at >= 0)
                {
                    Dir = line[(at + "RPGL_DIR=".Length)..];
                    return;
                }
            }
            Dispose();
            throw new InvalidOperationException("owner process exited without starting a server");
        }

        public void Kill9()
        {
            Signal("-KILL", _process.Id);
            _process.WaitForExit();
        }

        public void Dispose()
        {
            try
            {
                if (!_process.HasExited)
                {
                    _process.Kill();
                    _process.WaitForExit();
                }
            }
            catch (InvalidOperationException) { }
            _process.Dispose();
        }
    }

    private static void Signal(string sig, int pid)
    {
        using var kill = Process.Start("kill", new[] { sig, pid.ToString() })!;
        kill.WaitForExit();
        Assert.True(kill.ExitCode == 0, $"kill {sig} {pid} failed");
    }

    /// <summary>Running, and not a zombie.</summary>
    private static bool Alive(int pid)
    {
        if (pid <= 0)
            return false;
        if (OperatingSystem.IsLinux())
        {
            try
            {
                var stat = File.ReadAllText($"/proc/{pid}/stat");
                var close = stat.LastIndexOf(") ", StringComparison.Ordinal);
                return close < 0 || stat[close + 2] != 'Z';
            }
            catch (IOException) { return false; }
        }
        using var ps = Process.Start(new ProcessStartInfo("ps", new[] { "-o", "stat=", "-p", pid.ToString() })
        {
            RedirectStandardOutput = true,
        })!;
        var output = ps.StandardOutput.ReadToEnd().Trim();
        ps.WaitForExit();
        return ps.ExitCode == 0 && output.Length > 0 && !output.StartsWith('Z');
    }

    private static int PostmasterPid(string dir)
    {
        try
        {
            var first = File.ReadLines(System.IO.Path.Combine(dir, "postmaster.pid")).FirstOrDefault();
            return int.TryParse(first?.Trim(), out var pid) ? pid : 0;
        }
        catch (IOException) { return 0; }
    }

    /// <summary>A field of owner.json, as its raw text (strings without quotes).</summary>
    private static string OwnerField(string dir, string key)
    {
        var json = File.ReadAllText(System.IO.Path.Combine(dir, "owner.json"));
        var m = Regex.Match(json, $"\"{Regex.Escape(key)}\":\\s*\"?([^\",\\n}}]*)");
        Assert.True(m.Success, $"no {key} in owner.json");
        return m.Groups[1].Value.Trim();
    }

    private static bool WaitUntil(TimeSpan timeout, Func<bool> cond)
    {
        var sw = Stopwatch.StartNew();
        while (sw.Elapsed < timeout)
        {
            if (cond())
                return true;
            Thread.Sleep(100);
        }
        return cond();
    }

    private static EmbeddedPg StartIn(Root root, bool durable = false) =>
        EmbeddedPg.Start(new EmbeddedPgOptions { TempRoot = root.Path, Durable = durable });

    // ---- tests ----

    [Fact]
    public void KillNineOfTheOwnerStopsTheServerAndRemovesTheDir()
    {
        using var root = new Root("kill9");
        using var owner = new Owner(root, durable: false);
        var dir = owner.Dir;
        var pm = PostmasterPid(dir);
        _output.WriteLine($"owner {owner.Pid}, postmaster {pm}, dir {dir}");
        Assert.True(Alive(pm), "server should be running");
        Assert.True(Directory.Exists(dir));

        owner.Kill9();

        Assert.True(
            WaitUntil(TimeSpan.FromSeconds(15), () => !Alive(pm) && !Directory.Exists(dir)),
            $"after kill -9 of the owner: server alive={Alive(pm)}, dir exists={Directory.Exists(dir)}");
    }

    [Fact]
    public void DisposeStopsTheServerAndRemovesTheDir()
    {
        using var root = new Root("dispose");
        var pg = StartIn(root);
        var dir = pg.DataDir;
        var pm = PostmasterPid(dir);
        var watcher = int.Parse(OwnerField(dir, "watcher_pid"));
        Assert.False(pg.IsDurable);
        Assert.True(Alive(pm) && Alive(watcher));

        pg.Dispose();

        Assert.False(Alive(pm), "server still running after Dispose");
        Assert.False(Directory.Exists(dir), "dir still there after Dispose");
        Assert.True(WaitUntil(TimeSpan.FromSeconds(3), () => !Alive(watcher)), "watchdog still running");
        Assert.Throws<ObjectDisposedException>(() => pg.ConnectionString);
    }

    [Fact]
    public void SweepReclaimsOnlyDeadOwnersServers()
    {
        using var root = new Root("sweep");

        // A: owner alive (this process).
        using var live = StartIn(root);
        var liveDir = live.DataDir;

        // B: owner AND its watchdog dead — the case only the sweep can clean up.
        // C: durable, owner dead — must survive, and the sweep must leave it.
        // Both start before either dies: every start sweeps, and C's must not
        // get to B first.
        using var dead = new Owner(root, durable: false);
        using var durable = new Owner(root, durable: true);

        var deadDir = dead.Dir;
        var deadPm = PostmasterPid(deadDir);
        var deadWatcher = int.Parse(OwnerField(deadDir, "watcher_pid"));
        try
        {
            // Freeze B's watchdog first so it cannot act, then kill both.
            Signal("-STOP", deadWatcher);
            dead.Kill9();
            Signal("-KILL", deadWatcher);
            Assert.True(WaitUntil(TimeSpan.FromSeconds(5), () => !Alive(deadWatcher)));
        }
        finally
        {
            // Never leave a frozen watchdog behind if the above failed part-way.
            if (Alive(deadWatcher))
                Signal("-KILL", deadWatcher);
        }
        Assert.True(Alive(deadPm), "B's server should be orphaned, not stopped");

        var durableDir = durable.Dir;
        var durablePm = PostmasterPid(durableDir);
        Assert.Equal("true", OwnerField(durableDir, "durable"));
        durable.Kill9();

        // D: legacy — no owner.json. 0700, as mkdtemp makes them: the sweep
        // will not trust a dir others can write.
        var legacy = Path.Combine(root.Path, "rpgl_legacy");
        Directory.CreateDirectory(legacy);
        if (!OperatingSystem.IsWindows())
            File.SetUnixFileMode(legacy, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        File.WriteAllText(Path.Combine(legacy, "PG_VERSION"), "17\n");

        Thread.Sleep(500);
        Assert.True(Alive(durablePm), "a durable server must outlive its owner");

        var report = EmbeddedPg.Sweep(root.Path);
        Assert.NotNull(report); // null = another process sweeping this root: none should be
        _output.WriteLine(report.ToString());

        Assert.Equal(1, report.Reclaimed); // B
        Assert.Equal(1, report.Live);      // A
        Assert.Equal(0, report.Durable);   // C is rpgldur_*: outside the sweep altogether
        Assert.Equal(1, report.Legacy);    // D
        Assert.Equal(0, report.Failed);

        Assert.True(!Alive(deadPm) && !Directory.Exists(deadDir), "B reclaimed");
        Assert.True(Alive(PostmasterPid(liveDir)) && Directory.Exists(liveDir), "A untouched");
        Assert.True(Alive(durablePm) && Directory.Exists(durableDir), "C untouched");
        Assert.True(Directory.Exists(legacy), "D untouched");
    }

    [Fact]
    public void StopDirEndsADurableServer()
    {
        using var root = new Root("stopdir");
        var pg = StartIn(root, durable: true);
        Assert.True(pg.IsDurable);
        var dir = pg.DataDir;
        var pm = PostmasterPid(dir);
        Assert.Equal("0", OwnerField(dir, "watcher_pid")); // no watchdog

        pg.Dispose();
        Assert.True(Alive(pm), "disposing a durable handle detaches");

        EmbeddedPg.StopDir(dir);
        Assert.False(Alive(pm), "durable server stopped explicitly");
        Assert.False(Directory.Exists(dir), "its auto dir removed");
        Assert.Throws<PGliteException>(() => EmbeddedPg.StopDir(dir)); // no such dir any more
    }

    [Fact]
    public void StopEndsADurableServer()
    {
        using var root = new Root("stop");
        var pg = StartIn(root, durable: true);
        var dir = pg.DataDir;
        var pm = PostmasterPid(dir);

        pg.Stop();

        Assert.False(Alive(pm), "Stop() ends a durable server");
        Assert.False(Directory.Exists(dir));
        Assert.Throws<ObjectDisposedException>(() => pg.Port);
        pg.Dispose(); // no-op after Stop
    }

    [Fact]
    public void DetachLeavesTheServerRunning()
    {
        using var root = new Root("detach");
        var pg = StartIn(root);
        var dir = pg.DataDir;
        var pm = PostmasterPid(dir);

        pg.Detach();

        Assert.True(Alive(pm), "Detach() leaves the server running");
        EmbeddedPg.StopDir(dir);
        Assert.False(Alive(pm));
    }
}
