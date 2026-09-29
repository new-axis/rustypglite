namespace RustyPGlite;

/// <summary>
/// Options for <see cref="EmbeddedPg.Start(EmbeddedPgOptions)"/>. Every
/// property is optional; leaving it unset uses the default.
/// </summary>
public sealed record EmbeddedPgOptions
{
    /// <summary>Data directory. Null = a fresh auto directory under <see cref="TempRoot"/>.</summary>
    public string? DataDir { get; init; }

    /// <summary>Database name. Null = "postgres".</summary>
    public string? DbName { get; init; }

    /// <summary>Port number. Null = a free port chosen automatically.</summary>
    public int? Port { get; init; }

    /// <summary>Keep the data directory after the server stops. Default: false.</summary>
    public bool KeepData { get; init; }

    /// <summary>
    /// A long-lived server NOT bound to this process: no watchdog, not stopped
    /// on Dispose or when this process exits, never reclaimed by a sweep.
    /// End it with <see cref="EmbeddedPg.Stop"/> or <see cref="EmbeddedPg.StopDir"/>.
    /// Default: false.
    /// </summary>
    public bool Durable { get; init; }

    /// <summary>
    /// Where auto data directories go and what the start-up sweep scans.
    /// Null = $RUSTYPGLITE_TMPDIR, else /tmp.
    /// </summary>
    public string? TempRoot { get; init; }
}

/// <summary>
/// What <see cref="EmbeddedPg.Sweep"/> found under a temp root.
/// </summary>
/// <param name="Examined">rpgl_* directories looked at.</param>
/// <param name="Reclaimed">Servers of provably dead owners that were stopped and removed.</param>
/// <param name="Live">Left alone: the owner is still running.</param>
/// <param name="Durable">Left alone: started durable.</param>
/// <param name="Legacy">Left alone: no owner.json (an older rustypglite, or one still starting).</param>
/// <param name="Skipped">Left alone: not this process's to judge (another user, another PID namespace, unreadable, or writable by others).</param>
/// <param name="Failed">The owner is dead, but the server would not stop or the directory would not stay removed.</param>
public sealed record SweepReport(
    int Examined, int Reclaimed, int Live, int Durable, int Legacy, int Skipped, int Failed);
