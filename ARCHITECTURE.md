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
│  - Watchdog: server dies with its owner, even -9     │
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
│   ├── src/bin/rustypglite.rs        # CLI: `stop <dir>`, `sweep`
│   ├── tests/integration_test.rs     # 6 tests against real PG
│   └── tests/lifecycle_test.rs       # 8 tests: kill -9, sweep, durable, stop
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
| Rust lifecycle (no leaked servers) | 8 | All pass |
| .NET with Npgsql, incl. lifecycle | 13 | All pass |
| Node.js with node-pg, incl. lifecycle | 14 | All pass |

Tests cover: start/stop lifecycle, SELECT, CREATE TABLE, INSERT, UPDATE,
DELETE, parameterized queries, transactions (BEGIN/COMMIT/ROLLBACK), JSONB,
UUID (gen_random_uuid), multiple databases, multiple isolated instances —
and, in each binding, that `kill -9` of the owner stops the server and removes
its dir, that the sweep reclaims a dead owner's server but not a live, durable
or legacy one, and that the explicit stop ends a durable server.

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

The shim (`pg_shim.c`) manages the full postgres lifecycle:

1. **`rpgl_start()`**
   - Sweeps the temp root for dead owners' servers (see below)
   - `find_self_dir()` via `dladdr()` → locates `pg/bin/` next to the `.so`
   - `find_free_port()` → binds to port 0, reads assigned port
   - `mkdtemp(<temp root>/rpgl_XXXXXX)` (unless a data dir was given)
   - Forks the watchdog (unless durable)
   - `initdb` → creates the data directory (if needed)
   - Writes `owner.json` into the data dir
   - Appends speed-tuned settings to `postgresql.conf`:
     `fsync=off, synchronous_commit=off, full_page_writes=off, listen_addresses=''`
   - `pg_ctl start` → starts postgres on unix socket
   - `wait_for_ready()` → polls `pg_isready` or tries socket connect
   - Returns connection info

2. **`rpgl_stop()`**
   - Fast shutdown by signal (SIGINT → SIGQUIT → SIGKILL, bounded waits)
   - Removes the auto data dir (unless `keep_data` was set) — only if its
     `owner.json` still carries this start's token
   - Tells the watchdog "released" and reaps it
   - A durable server is stopped too: stopping is always explicit. To let go
     of a handle without stopping, `rpgl_detach()`. A handle from
     `rpgl_connect_existing()` is only freed — it never stops the server.

### Ownership: why no server outlives its owner

**pg_ctl daemonises the postmaster.** It forks, `setsid()`s, execs `/bin/sh -c
"exec postgres …"`, and exits, so the postmaster's parent is init (PPID 1)
*while its owner is alive*. PPID 1 is therefore **not** evidence of an
orphan, and an "orphan reaper" that kills PPID-1 postmasters kills live test
suites. Deciding by age or mtime is no better: it both deletes live servers'
dirs and leaves young orphans running.

Instead every server is bound to its owner — the process that called
`rpgl_start()` — twice over:

**1. A watchdog.** Right after `mkdtemp`, the shim forks a small `/bin/sh`
process whose stdin is one end of a `socketpair`; the owner holds the other
end, close-on-exec, so no other process (postgres included) inherits it. The
watchdog blocks reading it.

- On `rpgl_stop()` the owner stops the server, then writes `released`; the
  watchdog exits without touching anything.
- If the owner dies any other way — SIGKILL, a segfault, an abort, a
  cancelled CI job — the kernel closes its end, the watchdog reads EOF, runs
  `pg_ctl stop -m fast` (then `-m immediate`), and removes the data dir if
  rustypglite created it. It acts only if the dir's `owner.json` still carries
  its start's random token (or has none yet: the owner died during initdb), so
  it can never act on a dir that was since reused.
- The watchdog is in its own session and ignores HUP/INT/QUIT/TERM/PIPE, so a
  Ctrl-C to the test runner's process group, or a closed terminal, does not
  take it down with the owner. It holds none of the owner's other file
  descriptors (so it never keeps a test runner's output pipe open) and
  `chdir("/")`s.

Why not `PR_SET_PDEATHSIG`: with pg_ctl the postmaster is not the owner's
child, so it would never fire. Even without pg_ctl it fires when the forking
**thread** exits, not the process — in .NET and Node that is a thread-pool
thread, which would kill a healthy server mid-suite — it is Linux-only, and it
kills the server but cannot remove the data dir. Why not a thread or a signal
handler in the owner: they die with the owner (SIGKILL cannot be caught), and
stopping a server from a signal handler calls `fork`/`malloc`, which is not
async-signal-safe and can hang the very process being cleaned up. 0.1.x's
SIGINT/SIGTERM/SIGHUP handlers are gone for that reason; a normal exit still
stops servers via `atexit`.

**2. `owner.json`** in the data dir, written atomically after initdb:

```jsonc
{
  "version": 1,
  "owner_pid": 12345,
  "owner_start": "88190431",          // /proc/<pid>/stat field 22 (macOS: p_starttime)
  "owner_cwd": "/home/me/project",
  "unix_session": 12001,
  "session_id": "session_…",          // $RUSTYPGLITE_SESSION_ID, else Claude Code's, if set
  "session_var": "CLAUDE_CODE_BRIDGE_SESSION_ID",
  "boot_id": "5e1c…",                 // PID + start time only mean something within a boot
  "pid_ns": "pid:[4026531836]",       // …and within a PID namespace
  "watcher_pid": 12350,
  "watcher_start": "88190433",
  "durable": false,
  "owns_data_dir": true,
  "port": 45123,
  "created_at": 1790000000,
  "token": "9f2c…"                    // this start; guards against a reused dir name
}
```

**The sweep** (`rpgl_sweep`, run by every start, and by `rustypglite sweep`)
looks only at `<temp root>/rpgl_*` directories, and reclaims one — stops its
server by signal, then removes the dir if `owns_data_dir` — only when:

- the dir and its `owner.json` belong to this user, are not symlinks, and are
  not group/world-writable (anyone else could forge an `owner.json`);
- it is not `durable`;
- the owner is **dead**: a different boot, or no such PID, or the PID's start
  time differs (reused);
- and the watchdog is dead too (a live one is mid-cleanup).

Anything it cannot judge it leaves alone: no `owner.json` (a 0.1.x server, or
a start still in initdb), an unreadable or foreign file, or an owner in
another PID namespace (a container or sandbox, where "no such PID" proves
nothing). Sweeps are serialised per user and root by a non-blocking `flock`
on `rpgl_sweep.<uid>.lock`; a start that finds someone else sweeping does not
wait. Stopping a server checks, on Linux, that the PID in `postmaster.pid` is
a process whose cwd is that data dir, so a stale pidfile whose PID has been
reused never gets a signal sent to the wrong process.

**Durable servers** (`rpgl_options.durable`) are for servers that must not be
tied to any process lifetime — a dev database behind a fixed port, say. No
watchdog, no `atexit` stop, never swept; `rpgl_stop()` or `rpgl_stop_dir()`
(`rustypglite stop <dir>`) end them.

**The temp root** is `rpgl_options.temp_root`, else `$RUSTYPGLITE_TMPDIR`,
else `/tmp`. Tests use their own root so that their sweeps can only ever see
their own servers.

**What is not covered:** a server whose owner *and* watchdog were both
SIGKILLed stays up until the next start (or `rustypglite sweep`) on that
machine. A server started by 0.1.x stays up until someone stops it.

3. **`rpgl_exec_sql()` / `rpgl_create_database()`**
   - Shells out to `psql` / `createdb` for setup operations
   - Useful for DDL/migrations before handing off to a client library

## Build from source

```bash
# Prerequisites: gcc, rust, curl (that's it)

# 1. Build the native library (compiles only pg_shim.c)
cargo build --release

# 2. Set up Node.js package
cd rustypglite-node
npm install          # downloads PG 17.5 binaries automatically
npm test             # runs all 14 tests

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
