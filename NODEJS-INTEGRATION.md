# RustyPGlite — Node.js Integration Guide

## What it is

A bundled PostgreSQL 17.5 server you start from Node.js. Real postgres process, unix socket, standard `pg` Pool connection. No Docker, no install, no WASM.

## Quick start

```typescript
import { EmbeddedPg } from 'rustypglite';
import pg from 'pg';

const epg = EmbeddedPg.start();
const pool = new pg.Pool({
  host: epg.socketDir,
  port: epg.port,
  database: 'postgres',
  user: 'postgres',
});

const { rows } = await pool.query('SELECT 1 AS num');
console.log(rows[0].num); // 1

await pool.end();
epg.stop();
```

## Replacing PGlite in a TypeORM project

### What you can delete

The entire PGlite adapter layer:
- The whole PGlite adapter — in the codebase this was measured against, a
  316-line module of `PGlitePool`, an `EventEmitter` shim, a `driverShim`, a
  setup queue, search_path management and backup/restore logic. None of it is
  needed against a real server.

### What replaces it

```typescript
import { EmbeddedPg } from 'rustypglite';
import { DataSource } from 'typeorm';
import { SnakeNamingStrategy } from 'typeorm-naming-strategies';

let server: EmbeddedPg | null = null;

export async function createTestTypeORMClient(
  entities: any[] = [],
  schemaSQL: string[] = [],
) {
  // Start server once, reuse across domains
  if (!server) {
    server = EmbeddedPg.start();
    server.execSql('CREATE EXTENSION IF NOT EXISTS "uuid-ossp"');
  }

  // Each domain gets its own database — real isolation, no search_path hacks
  const dbName = `test_${Date.now()}_${Math.random().toString(36).slice(2, 8)}`;
  server.createDatabase(dbName);

  // Run schema DDL
  for (const sql of schemaSQL) {
    server.execSql(sql, dbName);
  }

  // Standard TypeORM DataSource — no driver shim needed
  const dataSource = new DataSource({
    type: 'postgres',
    host: server.socketDir,
    port: server.port,
    database: dbName,
    username: 'postgres',
    entities,
    synchronize: false,
    logging: ['error'],
    namingStrategy: new SnakeNamingStrategy(),
  });

  await dataSource.initialize();

  return {
    dataSource,
    cleanup: async () => {
      if (dataSource.isInitialized) {
        await dataSource.destroy();
      }
    },
  };
}
```

That's it. ~40 lines replaces ~316 lines.

### What goes away

| PGlite pattern | Why it existed | Not needed anymore |
|---|---|---|
| `PGlitePool extends EventEmitter` | PGlite doesn't speak pg wire protocol | Standard pg driver works |
| `driverShim` class | TypeORM needs a pg-compatible driver | TypeORM connects natively |
| `SET search_path TO "${schemaName}"` on every query | Shared PGlite instance, schema isolation | Each domain gets its own database |
| `setupQueue = setupQueue.then(...)` | Serialize DDL to prevent search_path interleaving | No shared state to interleave |
| `ROLLBACK` before each domain setup | Clean up aborted transaction state | Clean database each time |
| `backup()` / `restore()` via SELECT + DELETE + INSERT | Poor man's snapshot | Use `CREATE DATABASE ... TEMPLATE` (see below) |

### Fast test reset with template databases

Instead of the backup/restore pattern that SELECTs all rows, DELETEs them, and re-INSERTs:

```typescript
// Setup once
const epg = EmbeddedPg.start();
epg.execSql('CREATE EXTENSION IF NOT EXISTS "uuid-ossp"');

// Create a template with the schema
epg.createDatabase('template_myapp');
for (const sql of schemaSQL) {
  epg.execSql(sql, 'template_myapp');
}
// Seed any baseline data into template_myapp

// Per-test: clone the template (~10ms filesystem copy vs ~2s schema creation)
epg.execSql("CREATE DATABASE test_123 TEMPLATE template_myapp");
// test runs against test_123
// drop it when done
epg.execSql("DROP DATABASE test_123");
```

This is how production Postgres does fast database provisioning. PGlite can't do this because it runs in WASM with a virtual filesystem.

### vitest config changes

```typescript
// vitest.config.ts — no changes needed to isolate/hookTimeout
// But you can now use isolate: true if you want, since each test
// file gets its own database, not a shared PGlite instance
export default defineConfig({
  test: {
    globals: true,
    // isolate: true is now fine — no 800MB shared PGlite instance
  },
});
```

### Migration verification script

`scripts/verify-migrations.ts` uses PGlite directly. Replace with:

```typescript
import { EmbeddedPg } from 'rustypglite';
import pg from 'pg';

const epg = EmbeddedPg.start();
const pool = new pg.Pool({
  host: epg.socketDir,
  port: epg.port,
  database: 'postgres',
  user: 'postgres',
});

// Run migrations against real postgres
for (const migration of migrations) {
  await pool.query(migration.sql);
}

// Verify with pg_trgm etc.
await pool.query('CREATE EXTENSION IF NOT EXISTS pg_trgm');

await pool.end();
epg.stop();
```

## API reference

```typescript
import { EmbeddedPg } from 'rustypglite';

// Start server (~500ms)
const epg = EmbeddedPg.start();

// Connection info
epg.connectionString    // "host=/tmp/rpgl_xxx;port=NNNNN;database=postgres;username=postgres"
epg.connectionStringLibpq  // "host=/tmp/rpgl_xxx port=NNNNN database=postgres username=postgres"
epg.socketDir           // "/tmp/rpgl_xxx"
epg.port                // e.g. 37437
epg.dataDir             // "/tmp/rpgl_xxx"

// Create databases
epg.createDatabase('myapp');

// Run SQL via psql (good for DDL/extensions before app connects)
epg.execSql('CREATE EXTENSION IF NOT EXISTS "uuid-ossp"');
epg.execSql('CREATE TABLE ...', 'myapp');  // on specific database

// Stop server, delete data directory
epg.stop();
```

### Connecting with pg Pool

```typescript
import pg from 'pg';

// Option A: explicit params (recommended)
const pool = new pg.Pool({
  host: epg.socketDir,
  port: epg.port,
  database: 'postgres',
  user: 'postgres',
});

// Option B: libpq connection string
const pool = new pg.Pool({
  connectionString: epg.connectionStringLibpq,
});
```

### Connecting with TypeORM

```typescript
const dataSource = new DataSource({
  type: 'postgres',
  host: epg.socketDir,
  port: epg.port,
  database: 'mydb',
  username: 'postgres',
  entities: [...],
  synchronize: false,
});
await dataSource.initialize();
```

### Connecting with Knex

```typescript
const knex = require('knex')({
  client: 'pg',
  connection: {
    host: epg.socketDir,
    port: epg.port,
    database: 'postgres',
    user: 'postgres',
  },
});
```

### Connecting with Prisma

```typescript
// Set DATABASE_URL before Prisma client init
process.env.DATABASE_URL = `postgresql://postgres@localhost:${epg.port}/mydb?host=${epg.socketDir}`;
```

## PGlite vs RustyPGlite comparison

| | PGlite (WASM) | RustyPGlite (native) |
|---|---|---|
| Postgres version | 17.5 | 17.5 |
| Startup | ~1-2s (WASM compile) | ~500ms (initdb + start) |
| Query speed | WASM overhead | Native |
| Client library | Custom shim required | Standard `pg` |
| TypeORM integration | 250 lines of adapter | Zero — native driver |
| Memory | ~800MB shared instance | ~50MB per server |
| Extensions | Must compile to WASM | Normal PG extensions |
| Multiple databases | Not supported | `CREATE DATABASE` |
| Template databases | Not supported | `CREATE DATABASE ... TEMPLATE` |
| Transaction isolation | Works | Works |
| `search_path` hacks needed | Yes (shared instance) | No (separate databases) |
| Wire protocol | Emulated | Real |

## Prerequisites

Set these environment variables before running:

```bash
export RUSTYPGLITE_PG_DIR=/path/to/rustypglite/rustypglite-sys/pg-install
export LD_LIBRARY_PATH=$RUSTYPGLITE_PG_DIR/lib
```

The native library (`librustypglite.so`) must be findable. It looks in:
1. `RUSTYPGLITE_LIB` env var (explicit path to .so)
2. `../../target/release/librustypglite.so` (development)
3. `../native/librustypglite.so` (npm package layout)

## Troubleshooting

**"Failed to start embedded PostgreSQL"**
- Check `RUSTYPGLITE_PG_DIR` points to the pg-install directory containing `bin/postgres`
- Check `LD_LIBRARY_PATH` includes `$RUSTYPGLITE_PG_DIR/lib`

**pg Pool connection error**
- Use `host: epg.socketDir` not `host: 'localhost'` — there's no TCP listener
- Use `user: 'postgres'` — that's the superuser initdb creates

**uuid-ossp extension**
- `gen_random_uuid()` is built into PG 13+ with no extension needed
- If you specifically need `uuid_generate_v4()`, run `epg.execSql('CREATE EXTENSION IF NOT EXISTS "uuid-ossp"')`
- The uuid-ossp extension requires the ossp-uuid library at build time; if it's not available, use `gen_random_uuid()` instead — it's functionally identical for generating v4 UUIDs

**pg_trgm extension**
- Should work if contrib modules are built. Run `epg.execSql('CREATE EXTENSION IF NOT EXISTS pg_trgm')` to verify.
