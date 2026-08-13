# RustyPGlite — .NET Integration Guide

## What it is

A NuGet-installable PostgreSQL 17.5 server. Starts in ~500ms, runs on a unix socket, stops when disposed. No Docker, no install, no port conflicts.

## Quick start

```csharp
using var pg = EmbeddedPg.Start();
await using var conn = new NpgsqlConnection(pg.ConnectionString);
await conn.OpenAsync();
// real postgres, real npgsql, nothing special
```

## EF Core — the one-line swap

Replace your SQLite shim with:

```csharp
services.AddDbContext<AppDbContext>(options =>
{
    if (environment.IsTest())
        options.UseNpgsql(EmbeddedPg.Start().ConnectionString);
    else
        options.UseNpgsql(config.GetConnectionString("Postgres"));
});
```

No custom provider. No `UseRustyPgLite()`. Standard `UseNpgsql()`. Your existing migrations, LINQ queries, and change tracking all work because it IS Postgres.

## Lifecycle in tests

### xUnit with shared instance

```csharp
public class DatabaseFixture : IDisposable
{
    public EmbeddedPg Pg { get; } = EmbeddedPg.Start();
    public string ConnectionString => Pg.ConnectionString;

    public void Dispose() => Pg.Dispose();
}

[CollectionDefinition("Database")]
public class DatabaseCollection : ICollectionFixture<DatabaseFixture> { }

[Collection("Database")]
public class OrderTests
{
    private readonly string _connStr;

    public OrderTests(DatabaseFixture db) => _connStr = db.ConnectionString;

    [Fact]
    public async Task CanCreateOrder()
    {
        await using var conn = new NpgsqlConnection(_connStr);
        await conn.OpenAsync();
        // ...
    }
}
```

One postgres instance shared across the entire test collection. Starts once, stops at the end.

### xUnit with per-test isolation

```csharp
public class IsolatedTests : IDisposable
{
    private readonly EmbeddedPg _pg = EmbeddedPg.Start();

    [Fact]
    public async Task Test1()
    {
        await using var conn = new NpgsqlConnection(_pg.ConnectionString);
        // completely isolated — own data directory, own server
    }

    public void Dispose() => _pg.Dispose();
}
```

### Per-test database (fast isolation, shared server)

```csharp
public class FastIsolatedTests : IClassFixture<DatabaseFixture>
{
    private readonly DatabaseFixture _db;

    public FastIsolatedTests(DatabaseFixture db) => _db = db;

    [Fact]
    public async Task Test1()
    {
        // Create a unique database for this test
        var dbName = $"test_{Guid.NewGuid():N}"[..20];
        _db.Pg.CreateDatabase(dbName);

        var connStr = _db.Pg.ConnectionString
            .Replace("database=postgres", $"database={dbName}");

        await using var conn = new NpgsqlConnection(connStr);
        await conn.OpenAsync();
        // isolated database, shared server — fast
    }
}
```

## EF Core migrations

Run migrations against the embedded instance before tests:

```csharp
public class DatabaseFixture : IAsyncLifetime
{
    public EmbeddedPg Pg { get; private set; } = null!;
    public string ConnectionString => Pg.ConnectionString;

    public async Task InitializeAsync()
    {
        Pg = EmbeddedPg.Start();

        var options = new DbContextOptionsBuilder<AppDbContext>()
            .UseNpgsql(ConnectionString)
            .Options;

        await using var ctx = new AppDbContext(options);
        await ctx.Database.MigrateAsync();  // runs all EF migrations
    }

    public Task DisposeAsync()
    {
        Pg.Dispose();
        return Task.CompletedTask;
    }
}
```

## API reference

```csharp
// Start with defaults (temp dir, random port, silent)
using var pg = EmbeddedPg.Start();

// Properties
pg.ConnectionString  // "host=/tmp/rpgl_xxx;port=NNNNN;database=postgres;username=postgres"
pg.Port              // e.g. 55432
pg.SocketDir         // e.g. "/tmp/rpgl_xxx"
pg.DataDir           // e.g. "/tmp/rpgl_xxx"

// Create additional databases
pg.CreateDatabase("myapp_test");

// Run DDL/SQL directly (via psql, before Npgsql connects)
pg.ExecuteSql("CREATE EXTENSION IF NOT EXISTS pg_trgm");
pg.ExecuteSql("CREATE TABLE ...", dbName: "myapp_test");
```

## What works

Everything. It's real Postgres 17.5.

- UUID (`gen_random_uuid()` — built-in, no extension needed)
- JSONB, arrays, custom types
- Full-text search
- CTEs, window functions, lateral joins
- Transactions (BEGIN/COMMIT/ROLLBACK/SAVEPOINT)
- Multiple databases on one server
- Multiple isolated server instances in parallel
- `COPY`, prepared statements, cursors
- All Npgsql features (binary COPY, batching, multiplexing)

## What's different from production Postgres

These settings are tuned for testing speed, not durability:

```
fsync = off
synchronous_commit = off
full_page_writes = off
listen_addresses = ''     (unix socket only, no TCP)
```

Don't use this for data you care about keeping. It's for tests.

## Prerequisites

The native library (`librustypglite.so`) must be in the output directory. The NuGet package handles this automatically. For development, set:

```bash
export RUSTYPGLITE_PG_DIR=/path/to/rustypglite/rustypglite-sys/pg-install
export LD_LIBRARY_PATH=$RUSTYPGLITE_PG_DIR/lib
```

## Troubleshooting

**"Failed to start embedded PostgreSQL server"**
- Check `RUSTYPGLITE_PG_DIR` points to the pg-install directory
- Check `LD_LIBRARY_PATH` includes `$RUSTYPGLITE_PG_DIR/lib`

**Npgsql connection timeout**
- The server needs ~500ms to start. If creating in a static constructor or `IClassFixture`, this is handled before tests run.

**Port conflicts**
- Each instance finds a free port automatically. Multiple instances can run in parallel.

**uuid-ossp extension not available**
- Use `gen_random_uuid()` instead — it's built into Postgres 13+ with no extension required. If you need `uuid_generate_v4()` specifically, you'll need to build the uuid-ossp contrib module with the ossp-uuid library.
