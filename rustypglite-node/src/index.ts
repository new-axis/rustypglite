import koffi from 'koffi';
import path from 'path';
import fs from 'fs';
import { fileURLToPath } from 'url';

// ── Load native library ──

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const pkgRoot = path.join(__dirname, '..');

function findNativeLib(): string {
  const ext = process.platform === 'darwin' ? 'dylib' : 'so';
  const name = `librustypglite.${ext}`;

  const candidates = [
    // 1. Explicit env var
    process.env['RUSTYPGLITE_LIB'],
    // 2. Bundled next to the package: rustypglite/native/librustypglite.so
    //    (this is the primary path — native/ contains both the .so and pg/)
    path.join(pkgRoot, 'native', name),
    // 3. Development: cargo build output
    path.join(pkgRoot, '..', 'target', 'release', name),
    path.join(pkgRoot, '..', 'target', 'debug', name),
  ].filter(Boolean) as string[];

  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) return candidate;
  }

  throw new Error(
    `Could not find ${name}. Either:\n` +
    `  - Place it at ${path.join(pkgRoot, 'native', name)}\n` +
    `  - Set RUSTYPGLITE_LIB=/path/to/${name}\n` +
    `  - Run: cargo build --release`
  );
}

const lib = koffi.load(findNativeLib());

// ── FFI declarations ──

const rpglite_start = lib.func('rpglite_start', 'void*', []);
const rpglite_start_with = lib.func('rpglite_start_with', 'void*',
  ['str', 'str', 'int', 'int', 'int', 'str']);
const rpglite_connect_existing = lib.func('rpglite_connect_existing', 'void*', ['str']);
const rpglite_stop = lib.func('rpglite_stop', 'void', ['void*']);
const rpglite_stop_server = lib.func('rpglite_stop_server', 'void', ['void*']);
const rpglite_detach = lib.func('rpglite_detach', 'void', ['void*']);
const rpglite_stop_dir = lib.func('rpglite_stop_dir', 'int', ['str']);

const SweepResultStruct = koffi.struct('rpgl_sweep_result', {
  examined: 'int32',
  reclaimed: 'int32',
  live: 'int32',
  durable: 'int32',
  legacy: 'int32',
  skipped: 'int32',
  failed: 'int32',
});
const rpglite_sweep = lib.func('rpglite_sweep', 'int',
  ['str', koffi.out(koffi.pointer(SweepResultStruct))]);

// Shim return codes (pg_shim.h)
const RPGL_OK = 0;
const RPGL_ERR_ALREADY = -2;
const RPGL_ERR_STOP = -4;
const rpglite_connection_string = lib.func('rpglite_connection_string', 'str', ['void*']);
const rpglite_socket_dir = lib.func('rpglite_socket_dir', 'str', ['void*']);
const rpglite_port = lib.func('rpglite_port', 'int', ['void*']);
const rpglite_data_dir = lib.func('rpglite_data_dir', 'str', ['void*']);
const rpglite_create_database = lib.func('rpglite_create_database', 'int', ['void*', 'str']);
const rpglite_exec_sql = lib.func('rpglite_exec_sql', 'int', ['void*', 'str', 'str']);

// ── Public API ──

/** Options for {@link EmbeddedPg.start}. Every field is optional. */
export interface EmbeddedPgOptions {
  /**
   * Data directory. Default: a fresh `rpgl_XXXXXX` dir under `tempRoot`,
   * removed when the server stops. A dir you supply is never removed.
   */
  dataDir?: string;
  /** Database name. Default: "postgres". */
  dbName?: string;
  /** Port number. Default: a free port. */
  port?: number;
  /** Keep the data directory after stop. Default: false. */
  keepData?: boolean;
  /**
   * A long-lived server NOT bound to this process: no watchdog, not stopped
   * when this process exits or the handle is disposed, never reclaimed by a
   * sweep. End it with {@link EmbeddedPg.stop} or {@link stopDir}
   * (`rustypglite stop <dir>`). Default: false.
   */
  durable?: boolean;
  /**
   * Where auto data dirs go and what the start-up sweep scans.
   * Default: `$RUSTYPGLITE_TMPDIR`, else `/tmp`.
   */
  tempRoot?: string;
  /**
   * @deprecated Ignored. The native library is loaded once, when the module
   * is imported; set the `RUSTYPGLITE_LIB` environment variable instead.
   */
  nativeLibPath?: string;
}

/** What a {@link sweep} found, per `rpgl_*` dir under the temp root. */
export interface SweepReport {
  /** `rpgl_*` dirs looked at. */
  examined: number;
  /** Dead owner's servers stopped and removed. */
  reclaimed: number;
  /** Owner still alive: left alone. */
  live: number;
  /** Durable: left alone. */
  durable: number;
  /** No owner.json (older rustypglite, or not ours): left alone. */
  legacy: number;
  /** Could not be judged safely (e.g. another PID namespace): left alone. */
  skipped: number;
  /** Should have been reclaimed but could not be. */
  failed: number;
}

/**
 * Stop the server running in `dataDir`, from any process — the explicit stop
 * for scripts (`rustypglite stop <dir>`). Stops a durable server too.
 * Removes the dir only when its `owner.json` says rustypglite created it;
 * never a data dir you supplied. Throws if the server would not stop or the
 * dir is not a data dir of this user's.
 */
export function stopDir(dataDir: string): void {
  const rc = rpglite_stop_dir(dataDir);
  if (rc === RPGL_OK) return;
  if (rc === RPGL_ERR_STOP) {
    throw new Error(`the server in ${dataDir} did not stop`);
  }
  throw new Error(`${dataDir} is not a data dir of yours`);
}

/**
 * Reclaim servers whose owner is provably dead, under `tempRoot` (default
 * `$RUSTYPGLITE_TMPDIR`, else `/tmp`). Touches only `rpgl_*` dirs whose
 * `owner.json` shows the owner and its watchdog are both gone — never a live
 * owner's dir, a durable one, or one without `owner.json`. Every start already
 * does this; call it to report, or to sweep without starting.
 *
 * Returns `null` if another process is sweeping this root right now; throws
 * if the root is missing or unwritable.
 */
export function sweep(tempRoot?: string): SweepReport | null {
  const out: Record<string, number> = {};
  const rc = rpglite_sweep(tempRoot ?? null, out);
  if (rc === RPGL_ERR_ALREADY) return null;
  if (rc !== RPGL_OK) {
    throw new Error('could not sweep (temp root missing or unwritable?)');
  }
  return {
    examined: out.examined,
    reclaimed: out.reclaimed,
    live: out.live,
    durable: out.durable,
    legacy: out.legacy,
    skipped: out.skipped,
    failed: out.failed,
  };
}

/**
 * An embedded PostgreSQL server.
 *
 * Starts a real Postgres 17.5 process on a unix socket.
 * Use the connectionString with any Postgres client (pg, Knex, Prisma, TypeORM).
 *
 * @example
 * ```typescript
 * import { EmbeddedPg } from 'rustypglite';
 * import pg from 'pg';
 *
 * const epg = EmbeddedPg.start();
 * const pool = new pg.Pool({ connectionString: epg.connectionString });
 * const { rows } = await pool.query('SELECT 1 AS num');
 * console.log(rows[0].num); // 1
 * epg.stop();
 * ```
 *
 * No leaked servers: a server never outlives the process that started it.
 * `stop()` or disposing the handle stops it; so does a normal exit; and if the
 * process is killed outright (SIGKILL, a crash, a cancelled test run) a small
 * watchdog stops the server and removes its data dir. For a server that must
 * outlive its starter, pass `{ durable: true }`.
 */
export class EmbeddedPg {
  private handle: unknown;
  private released = false;
  private readonly durable: boolean;

  private constructor(handle: unknown, durable: boolean) {
    this.handle = handle;
    this.durable = durable;
  }

  /**
   * Start an embedded PostgreSQL server.
   * Runs initdb + starts postgres on a unix socket with a random port.
   * Tuned for testing speed (fsync=off, synchronous_commit=off).
   * Takes ~500ms on first call.
   */
  static start(options?: EmbeddedPgOptions): EmbeddedPg {
    const o = options ?? {};
    const useDefaults =
      o.dataDir === undefined && o.dbName === undefined && o.port === undefined &&
      !o.keepData && !o.durable && o.tempRoot === undefined;
    const handle = useDefaults
      ? rpglite_start()
      : rpglite_start_with(
          o.dataDir ?? null,
          o.dbName ?? null,
          o.port ?? 0,
          o.keepData ? 1 : 0,
          o.durable ? 1 : 0,
          o.tempRoot ?? null,
        );
    if (!handle) {
      throw new Error(
        'Failed to start embedded PostgreSQL. ' +
        'Check that native/pg/bin/postgres exists (or RUSTYPGLITE_PG_DIR is set).'
      );
    }
    return new EmbeddedPg(handle, !!o.durable);
  }

  /** See the module-level {@link stopDir}. */
  static stopDir(dataDir: string): void {
    stopDir(dataDir);
  }

  /** See the module-level {@link sweep}. */
  static sweep(tempRoot?: string): SweepReport | null {
    return sweep(tempRoot);
  }

  /**
   * Connect to an already-running instance by its data directory.
   * Reads postmaster.pid to discover port and socket. Does not start a server.
   *
   * Use this for shared-server-across-workers:
   *   - Main process: EmbeddedPg.start(), write dataDir to env/file
   *   - Workers: EmbeddedPg.connectExisting(dataDir)
   */
  static connectExisting(dataDir: string): EmbeddedPg {
    const handle = rpglite_connect_existing(dataDir);
    if (!handle) {
      throw new Error(
        `Failed to connect to existing PostgreSQL at ${dataDir}. ` +
        'Check that postmaster.pid exists and the server is running.'
      );
    }
    return new EmbeddedPg(handle, false);
  }

  /**
   * Connection string for use with any Postgres client.
   *
   * For node-pg Pool: `new Pool({ connectionString: pg.connectionString })`
   * For Knex: `knex({ connection: pg.connectionString })`
   * For TypeORM: `{ type: 'postgres', url: pg.connectionString }`
   */
  get connectionString(): string {
    this.throwIfStopped();
    return rpglite_connection_string(this.handle) ?? '';
  }

  /**
   * libpq-style connection string (space-separated key=value).
   * Use this with node-pg Pool which expects libpq format.
   */
  get connectionStringLibpq(): string {
    this.throwIfStopped();
    const cs = rpglite_connection_string(this.handle) ?? '';
    // Convert from semicolon format to space format
    return cs.replace(/;/g, ' ');
  }

  /** Unix socket directory path. */
  get socketDir(): string {
    this.throwIfStopped();
    return rpglite_socket_dir(this.handle) ?? '';
  }

  /** Port number. */
  get port(): number {
    this.throwIfStopped();
    return rpglite_port(this.handle);
  }

  /** Data directory path. */
  get dataDir(): string {
    this.throwIfStopped();
    return rpglite_data_dir(this.handle) ?? '';
  }

  /**
   * Create a new database on this server.
   */
  createDatabase(name: string): void {
    this.throwIfStopped();
    const rc = rpglite_create_database(this.handle, name);
    if (rc !== 0) {
      throw new Error(`Failed to create database '${name}'`);
    }
  }

  /**
   * Execute SQL directly (via psql).
   * Useful for running DDL/migrations before handing off to a client library.
   */
  execSql(sql: string, dbName?: string): void {
    this.throwIfStopped();
    const rc = rpglite_exec_sql(this.handle, dbName ?? null as any, sql);
    if (rc !== 0) {
      throw new Error(`SQL execution failed: ${sql.slice(0, 100)}`);
    }
  }

  /** Whether this server was started with `durable: true`. */
  get isDurable(): boolean {
    return this.durable;
  }

  /**
   * Stop the server and clean up the data directory (unless it was supplied
   * or `keepData` was set). Stops a durable server too: this is the explicit
   * stop. For a handle from {@link connectExisting}, only lets go of it.
   */
  stop(): void {
    if (!this.released) {
      this.released = true;
      rpglite_stop_server(this.handle);
    }
  }

  /**
   * Let go of the handle without stopping the server. A durable server keeps
   * running after this process exits; a non-durable one is still stopped
   * when this process exits.
   */
  detach(): void {
    if (!this.released) {
      this.released = true;
      rpglite_detach(this.handle);
    }
  }

  /**
   * For `using` / cleanup patterns: stops the server — unless it is durable,
   * which by definition outlives its handle: then it detaches.
   */
  [Symbol.dispose](): void {
    if (!this.released) {
      this.released = true;
      rpglite_stop(this.handle); // native "drop": stop, or detach if durable
    }
  }

  private throwIfStopped(): void {
    if (this.released) {
      throw new Error('EmbeddedPg instance has been stopped or detached');
    }
  }
}

export default EmbeddedPg;
