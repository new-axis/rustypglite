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
const rpglite_connect_existing = lib.func('rpglite_connect_existing', 'void*', ['str']);
const rpglite_stop = lib.func('rpglite_stop', 'void', ['void*']);
const rpglite_connection_string = lib.func('rpglite_connection_string', 'str', ['void*']);
const rpglite_socket_dir = lib.func('rpglite_socket_dir', 'str', ['void*']);
const rpglite_port = lib.func('rpglite_port', 'int', ['void*']);
const rpglite_data_dir = lib.func('rpglite_data_dir', 'str', ['void*']);
const rpglite_create_database = lib.func('rpglite_create_database', 'int', ['void*', 'str']);
const rpglite_exec_sql = lib.func('rpglite_exec_sql', 'int', ['void*', 'str', 'str']);

// ── Public API ──

export interface EmbeddedPgOptions {
  /** Override the native library path */
  nativeLibPath?: string;
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
 */
export class EmbeddedPg {
  private handle: unknown;
  private stopped = false;

  private constructor(handle: unknown) {
    this.handle = handle;
  }

  /**
   * Start an embedded PostgreSQL server.
   * Runs initdb + starts postgres on a unix socket with a random port.
   * Tuned for testing speed (fsync=off, synchronous_commit=off).
   * Takes ~500ms on first call.
   */
  static start(): EmbeddedPg {
    const handle = rpglite_start();
    if (!handle) {
      throw new Error(
        'Failed to start embedded PostgreSQL. ' +
        'Check that native/pg/bin/postgres exists.'
      );
    }
    return new EmbeddedPg(handle);
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
    return new EmbeddedPg(handle);
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

  /**
   * Stop the server and clean up the data directory.
   */
  stop(): void {
    if (!this.stopped) {
      this.stopped = true;
      rpglite_stop(this.handle);
    }
  }

  /**
   * Alias for stop() — for use with try/finally or cleanup patterns.
   */
  [Symbol.dispose](): void {
    this.stop();
  }

  private throwIfStopped(): void {
    if (this.stopped) {
      throw new Error('EmbeddedPg instance has been stopped');
    }
  }
}

export default EmbeddedPg;
