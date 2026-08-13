# RustyPGlite Architecture

## What it is

A self-contained PostgreSQL 17.5 server bundled as a library. Start a real
Postgres process from Node.js, .NET, or Rust with one function call. Standard
client libraries (pg, Npgsql, psycopg2) connect via unix socket. No Docker,
no install, no environment variables.

## How it works

```
┌──────────────────────────────────────────────────────┐
│  Your app / test runner                              │
│                                                      │
│  Node.js:  const pg = EmbeddedPg.start()             │
│  .NET:     using var pg = EmbeddedPg.Start()         │
│  Rust:     let pg = EmbeddedPg::start()              │
│                                                      │
│  → pg.connectionString / pg.socketDir / pg.port      │
│  → standard pg Pool / NpgsqlConnection connects      │
├──────────────────────────────────────────────────────┤
│  librustypglite.so (418KB)                           │
│  - C shim: finds pg/ next to itself via dladdr()     │
│  - Runs initdb if data dir doesn't exist             │
│  - Starts postgres via pg_ctl on a unix socket       │
│  - Waits for pg_isready, returns connection info     │
│  - Stops and cleans up on rpgl_stop()                │
├──────────────────────────────────────────────────────┤
│  pg/ (bundled alongside .so, ~19MB)                  │
│  ├── bin/  postgres, initdb, pg_ctl, psql, ...       │
│  ├── lib/  libpq.so.5, extensions                    │
│  └── share/postgresql/  postgres.bki, configs        │
└──────────────────────────────────────────────────────┘
```

The native library uses `dladdr()` to find its own location on disk, then
looks for `pg/bin/` next to itself. No environment variables needed.

## Project structure

```
rustypglite/
├── Cargo.toml                        # Rust workspace
│
├── rustypglite-sys/                  # C shim (compiles pg_shim.c only)
│   ├── build.rs                      # 15 lines: cc::Build + link dl
│   ├── src/lib.rs                    # FFI declarations
│   └── shim/
│       ├── pg_shim.h                 # C API (start/stop/connstr)
│       └── pg_shim.c                 # Lifecycle manager + dlopen(libpq)
│
├── rustypglite/                      # Rust wrapper (cdylib + rlib)
│   ├── src/lib.rs                    # EmbeddedPg API + C ABI exports
│   └── tests/integration_test.rs     # 6 tests against real PG
│
├── rustypglite-node/                 # Node.js package
│   ├── package.json                  # postinstall downloads PG binaries
│   ├── scripts/download-pg.sh        # Downloads zonkyio PG for platform
│   ├── src/index.ts                  # EmbeddedPg class via koffi FFI
│   ├── src/test.ts                   # 7 tests with node-pg
│   └── native/                       # .so + pg/ (auto-populated)
│
├── rustypglite-csharp/               # .NET package
│   ├── RustyPGlite/
│   │   ├── NativeMethods.cs          # P/Invoke declarations
│   │   ├── PGliteDatabase.cs         # EmbeddedPg class
│   │   └── PGliteException.cs
│   └── RustyPGlite.Tests/
│       └── BasicTests.cs             # 7 tests with Npgsql
│
├── scripts/bundle.sh                 # Packages .so + pg/ for distribution
├── DOTNET-INTEGRATION.md             # .NET developer guide
└── NODEJS-INTEGRATION.md             # Node.js developer guide
```

## Test results

### Unit / integration tests

| Suite | Tests | Status |
|-------|-------|--------|
| Rust integration (real PG) | 6 | All pass |
| .NET with Npgsql | 7 | All pass |
| Node.js with node-pg | 7 | All pass |

Tests cover: start/stop lifecycle, SELECT, CREATE TABLE, INSERT, UPDATE,
DELETE, parameterized queries, transactions (BEGIN/COMMIT/ROLLBACK), JSONB,
UUID (gen_random_uuid), multiple databases, multiple isolated instances.

### Production benchmark

Replaced PGlite (WASM Postgres) with RustyPGlite in a real codebase with
5,662 tests across 10 domains, 163 tables, and 159 migrations.

| Configuration | Wall clock | Result |
|---|---|---|
| PGlite WASM (baseline) | 39.16s | 5662 pass |
| **RustyPGlite (native)** | **22.38s** | **5662 pass** |

**1.75x faster.** The adapter code shrank from 316 lines of PGlite shim
(EventEmitter, driver adapter, search_path management) to 80 lines of
standard TypeORM DataSource configuration.

### What we tried that didn't help

| Idea | Result | Why |
|---|---|---|
| `isolate: true` (vitest) | 3.6x slower | Re-parses Prisma schemas, re-creates DataSources per file |
| Template databases | 5.3x slower | TypeORM DataSource reconnect cost (~100ms) × thousands of tests exceeds DELETE cost |

**The winning pattern:** shared postgres instance with `isolate: false`,
cached infrastructure per worker, DELETE/INSERT restore between tests.
Same pattern as PGlite, but native speed instead of WASM.

## How the C shim works

The shim (`pg_shim.c`, ~400 lines) manages the full postgres lifecycle:

1. **`rpgl_start()`**
   - `find_self_dir()` via `dladdr()` → locates `pg/bin/` next to the `.so`
   - `find_free_port()` → binds to port 0, reads assigned port
   - `initdb` → creates data directory in tmpdir (if needed)
   - Appends speed-tuned settings to `postgresql.conf`:
     `fsync=off, synchronous_commit=off, full_page_writes=off, listen_addresses=''`
   - `pg_ctl start` → starts postgres on unix socket
   - `wait_for_ready()` → polls `pg_isready` or tries socket connect
   - Returns connection info

2. **`rpgl_stop()`**
   - `pg_ctl stop -m fast` → clean shutdown
   - `rm -rf` tmpdir (unless `keep_data` was set)

3. **`rpgl_exec_sql()` / `rpgl_create_database()`**
   - Shells out to `psql` / `createdb` for setup operations
   - Useful for DDL/migrations before handing off to a client library

## Build from source

```bash
# Prerequisites: gcc, rust, curl (that's it)

# 1. Build the native library (compiles only pg_shim.c, ~400 lines)
cargo build --release

# 2. Set up Node.js package
cd rustypglite-node
npm install          # downloads PG 17.5 binaries automatically
npm test             # runs all 7 tests

# 3. Set up .NET package
cd rustypglite-csharp
dotnet test RustyPGlite.Tests
```

No flex, bison, m4, perl, or make needed. PostgreSQL is not compiled from
source — prebuilt binaries are downloaded from Maven Central.

## PostgreSQL binary source

Binaries come from [zonkyio/embedded-postgres-binaries](https://github.com/zonkyio/embedded-postgres-binaries),
the same source used by embedded-postgres-go and embedded-postgres (Java).
Hosted on Maven Central, no authentication required.

Available platforms: linux-amd64, linux-arm64, linux-amd64-alpine,
linux-arm64-alpine, darwin-amd64, darwin-arm64, windows-amd64.

### Updating PostgreSQL version

```bash
export RUSTYPGLITE_PG_VERSION=17.6.0
rm -rf rustypglite-node/native/pg
cd rustypglite-node && bash scripts/download-pg.sh && npm test
```

One env var, one command. No source code changes needed.

---

## Future development

### Near-term improvements

**Static linking of libpq** — Currently the bundle includes `libpq.so.5` and
other shared libraries in `pg/lib/`. Statically linking these into the postgres
binaries would reduce the bundle to just `pg/bin/` + `pg/share/` and eliminate
`LD_LIBRARY_PATH` concerns for the child processes.

**Smaller bundle via strip + selective packaging** — The `pg/bin/postgres`
binary is 11MB with debug symbols. `strip` and removing unnecessary locale
conversions could bring the total bundle under 10MB.

**Pre-built bundles for all platforms** — CI pipeline (GitHub Actions) that
builds for linux-x64, linux-arm64, darwin-arm64, darwin-x64, windows-x64.
Published as platform-specific npm optional dependencies and NuGet runtime
packages. Same pattern as esbuild, sharp, turbo.

**Windows TCP fallback** — Windows doesn't support unix sockets for Postgres.
The shim would use `listen_addresses = '127.0.0.1'` with a random port
instead. ~5 line change in `pg_shim.c`.

**npm/NuGet publish workflow** — Automated publishing from CI. The npm package
would use the optional dependency pattern:
```
@rustypglite/linux-x64
@rustypglite/darwin-arm64
...
```

### Possible future directions

**Template database pooling** — Pre-create N template databases with schema
applied, hand them out to tests, recycle via DROP + re-clone. This didn't help
in our TypeORM benchmark (reconnect cost dominated), but would help frameworks with
cheaper connection management.

**Snapshot via filesystem copy** — Instead of DELETE/INSERT restore, `cp -a`
the data directory to a snapshot after schema setup, then `cp -a` back before
each test. Faster than DELETE for large schemas with many tables. Requires
stopping/restarting postgres per restore, so only wins if schema setup cost
exceeds restart cost.

**Connection pooling proxy** — Run pgbouncer or a lightweight proxy alongside
the embedded postgres to handle connection pooling. Would help frameworks
that create many short-lived connections.

**Contrib extensions** — Build and bundle commonly-needed extensions:
pg_trgm (text search), hstore, pg_stat_statements, postgis. Currently only
plpgsql and pgcrypto are included.

### What we evaluated and decided against

**In-process Postgres (embedding the engine directly)** — We prototyped this
(patches exist in `patches/`). Decided against because:
- Single instance per process — can't run parallel tests
- Crash in Postgres kills the host process
- `longjmp` across FFI boundaries is undefined behavior
- No standard client library support (Npgsql, node-pg need a socket)
- The subprocess approach is simpler, safer, and supports the same use cases

**Custom EF Core provider** — Considered building `options.UseRustyPgLite()`.
Unnecessary — standard `options.UseNpgsql(pg.ConnectionString)` works because
we ARE a real Postgres server.

**SQLite compatibility shim** — The .NET team had a SQLite-based PGlite shim
with 46/48 tests passing. Replaced by RustyPGlite with 5662/5662 passing
because it's real Postgres, not an emulation layer.
