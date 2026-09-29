import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { spawn, spawnSync, type ChildProcess } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { EmbeddedPg, stopDir, sweep } from './index.js';
import pg from 'pg';

describe('EmbeddedPg', () => {
  it('starts and stops', () => {
    const epg = EmbeddedPg.start();
    assert.ok(epg.connectionString.length > 0, 'connection string should not be empty');
    assert.ok(epg.port > 0, 'port should be positive');
    assert.ok(epg.socketDir.length > 0, 'socket dir should not be empty');
    console.log(`  Connection: ${epg.connectionStringLibpq}`);
    console.log(`  Port: ${epg.port}`);
    epg.stop();
  });

  it('connects with node-pg Pool', async () => {
    const epg = EmbeddedPg.start();
    try {
      const pool = new pg.Pool({
        host: epg.socketDir,
        port: epg.port,
        database: 'postgres',
        user: 'postgres',
      });

      const { rows } = await pool.query('SELECT 1 AS num');
      assert.equal(rows[0].num, 1);

      await pool.end();
    } finally {
      epg.stop();
    }
  });

  it('full CRUD with parameterized queries', async () => {
    const epg = EmbeddedPg.start();
    try {
      const pool = new pg.Pool({
        host: epg.socketDir,
        port: epg.port,
        database: 'postgres',
        user: 'postgres',
      });

      // CREATE TABLE
      await pool.query(`
        CREATE TABLE users (
          id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
          email TEXT NOT NULL UNIQUE,
          name TEXT NOT NULL,
          metadata JSONB NOT NULL DEFAULT '{}',
          created_at TIMESTAMPTZ NOT NULL DEFAULT now()
        )
      `);

      // INSERT with RETURNING
      const { rows: [inserted] } = await pool.query(
        'INSERT INTO users (email, name, metadata) VALUES ($1, $2, $3) RETURNING id, email',
        ['alice@example.com', 'Alice', JSON.stringify({ role: 'admin' })]
      );
      assert.ok(inserted.id, 'should return UUID');
      assert.equal(inserted.email, 'alice@example.com');

      // SELECT
      const { rows } = await pool.query('SELECT name, metadata FROM users WHERE email = $1', ['alice@example.com']);
      assert.equal(rows[0].name, 'Alice');
      assert.deepEqual(rows[0].metadata, { role: 'admin' });

      // UPDATE
      const { rowCount } = await pool.query('UPDATE users SET name = $1 WHERE email = $2', ['Alice Smith', 'alice@example.com']);
      assert.equal(rowCount, 1);

      // DELETE
      const del = await pool.query('DELETE FROM users WHERE email = $1', ['alice@example.com']);
      assert.equal(del.rowCount, 1);

      await pool.end();
    } finally {
      epg.stop();
    }
  });

  it('transactions with rollback', async () => {
    const epg = EmbeddedPg.start();
    try {
      const pool = new pg.Pool({
        host: epg.socketDir,
        port: epg.port,
        database: 'postgres',
        user: 'postgres',
      });

      await pool.query('CREATE TABLE accounts (id INT PRIMARY KEY, balance INT)');
      await pool.query('INSERT INTO accounts VALUES (1, 100), (2, 200)');

      // Transaction that rolls back
      const client = await pool.connect();
      try {
        await client.query('BEGIN');
        await client.query('UPDATE accounts SET balance = 0 WHERE id = 1');
        await client.query('ROLLBACK');
      } finally {
        client.release();
      }

      // Balance unchanged
      const { rows: [a1] } = await pool.query('SELECT balance FROM accounts WHERE id = 1');
      assert.equal(a1.balance, 100);

      // Transaction that commits
      const client2 = await pool.connect();
      try {
        await client2.query('BEGIN');
        await client2.query('UPDATE accounts SET balance = balance - 50 WHERE id = 1');
        await client2.query('UPDATE accounts SET balance = balance + 50 WHERE id = 2');
        await client2.query('COMMIT');
      } finally {
        client2.release();
      }

      const { rows } = await pool.query('SELECT balance FROM accounts ORDER BY id');
      assert.equal(rows[0].balance, 50);
      assert.equal(rows[1].balance, 250);

      await pool.end();
    } finally {
      epg.stop();
    }
  });

  it('multiple databases', async () => {
    const epg = EmbeddedPg.start();
    try {
      epg.createDatabase('db1');
      epg.createDatabase('db2');

      const pool1 = new pg.Pool({ host: epg.socketDir, port: epg.port, database: 'db1', user: 'postgres' });
      const pool2 = new pg.Pool({ host: epg.socketDir, port: epg.port, database: 'db2', user: 'postgres' });

      await pool1.query('CREATE TABLE t (val TEXT)');
      await pool1.query("INSERT INTO t VALUES ('from db1')");

      await pool2.query('CREATE TABLE t (val TEXT)');
      await pool2.query("INSERT INTO t VALUES ('from db2')");

      const r1 = await pool1.query('SELECT val FROM t');
      const r2 = await pool2.query('SELECT val FROM t');

      assert.equal(r1.rows[0].val, 'from db1');
      assert.equal(r2.rows[0].val, 'from db2');

      await pool1.end();
      await pool2.end();
    } finally {
      epg.stop();
    }
  });

  it('execSql for DDL setup', () => {
    const epg = EmbeddedPg.start();
    try {
      epg.execSql('CREATE TABLE setup_test (id SERIAL, name TEXT)');
      epg.execSql("INSERT INTO setup_test (name) VALUES ('works')");
      // If we get here without throwing, psql execution works
    } finally {
      epg.stop();
    }
  });

  it('multiple isolated instances', async () => {
    const epg1 = EmbeddedPg.start();
    const epg2 = EmbeddedPg.start();
    try {
      assert.notEqual(epg1.port, epg2.port);

      const pool1 = new pg.Pool({ host: epg1.socketDir, port: epg1.port, database: 'postgres', user: 'postgres' });
      const pool2 = new pg.Pool({ host: epg2.socketDir, port: epg2.port, database: 'postgres', user: 'postgres' });

      await pool1.query('CREATE TABLE shared (id INT)');
      await pool2.query('CREATE TABLE shared (id INT)');

      await pool1.query('INSERT INTO shared VALUES (1)');
      await pool2.query('INSERT INTO shared VALUES (2)');

      const r1 = await pool1.query('SELECT id FROM shared');
      const r2 = await pool2.query('SELECT id FROM shared');

      assert.equal(r1.rows[0].id, 1);
      assert.equal(r2.rows[0].id, 2);

      await pool1.end();
      await pool2.end();
    } finally {
      epg1.stop();
      epg2.stop();
    }
  });
});

// ── No leaked servers ──
//
// Every test here works in its own temp root (never the real /tmp/rpgl_*), so
// the sweeps cannot touch anyone else's servers. The roots' parent is
// $RUSTYPGLITE_TEST_ROOT, else /tmp — keep it short: the unix socket path
// <root>/rpgl_XXXXXX/.s.PGSQL.NNNNN must stay under ~100 bytes.
//
// "Owner" processes that get killed are real Node processes using this
// binding: src/owner-probe.ts, run in-process with `node --import tsx` so the
// PID we SIGKILL is the process that started the server.

const here = path.dirname(fileURLToPath(import.meta.url));
const compiled = import.meta.url.endsWith('.js');
const probe = path.join(here, compiled ? 'owner-probe.js' : 'owner-probe.ts');
const cli = path.join(here, compiled ? 'cli.js' : 'cli.ts');
const loader = compiled ? [] : ['--import', 'tsx'];

/** Running, and not a zombie. */
function alive(pid: number): boolean {
  if (!pid) return false;
  try {
    const stat = fs.readFileSync(`/proc/${pid}/stat`, 'utf8');
    return stat.slice(stat.lastIndexOf(') ') + 2)[0] !== 'Z';
  } catch {
    return false;
  }
}

function cmdline(pid: number): string {
  try {
    return fs.readFileSync(`/proc/${pid}/cmdline`, 'utf8').split('\0').join(' ');
  } catch {
    return '';
  }
}

function postmasterPid(dir: string): number {
  try {
    return parseInt(fs.readFileSync(path.join(dir, 'postmaster.pid'), 'utf8').split('\n')[0], 10) || 0;
  } catch {
    return 0;
  }
}

function ownerJson(dir: string): Record<string, unknown> {
  return JSON.parse(fs.readFileSync(path.join(dir, 'owner.json'), 'utf8'));
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function waitUntil(ms: number, cond: () => boolean): Promise<boolean> {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    if (cond()) return true;
    await sleep(100);
  }
  return cond();
}

/** A temp root of its own, and everything started under it. */
class Root {
  readonly path: string;
  private owners: ChildProcess[] = [];
  private watchers: { pid: number; dir: string }[] = [];

  constructor(name: string) {
    const base = process.env['RUSTYPGLITE_TEST_ROOT'] ?? '/tmp';
    this.path = path.join(base, `rt${process.pid}-${name}`);
    fs.mkdirSync(this.path, { recursive: true });
  }

  /** Start an owner process; resolves with its pid and data dir. */
  async owner(durable: boolean): Promise<{ child: ChildProcess; dir: string }> {
    const child = spawn(process.execPath, [...loader, probe, this.path, durable ? 'durable' : ''], {
      stdio: ['ignore', 'pipe', 'inherit'],
    });
    this.owners.push(child);
    const dir = await new Promise<string>((resolve, reject) => {
      let buf = '';
      const timer = setTimeout(() => reject(new Error('owner probe did not start a server in 60 s')), 60_000);
      child.stdout!.on('data', (d: Buffer) => {
        buf += d.toString();
        const m = buf.match(/RPGL_DIR=(.*)\n/);
        if (m) {
          clearTimeout(timer);
          resolve(m[1]);
        }
      });
      child.on('exit', (code) => {
        clearTimeout(timer);
        reject(new Error(`owner probe exited (${code}) without starting a server`));
      });
    });
    child.stdout!.resume();
    return { child, dir };
  }

  /** Remember a watchdog we froze, so cleanup can kill it if a test fails. */
  froze(pid: number, dir: string): void {
    this.watchers.push({ pid, dir });
  }

  /** Leave nothing running: kill our owners and frozen watchdogs, stop every server, remove the root. */
  cleanup(): void {
    for (const c of this.owners) {
      if (c.exitCode === null && c.signalCode === null && c.pid && alive(c.pid)) {
        c.kill('SIGKILL');
      }
    }
    for (const w of this.watchers) {
      const cl = cmdline(w.pid);
      if (alive(w.pid) && cl.includes('rustypglite-watchdog') && cl.includes(w.dir)) {
        process.kill(w.pid, 'SIGKILL');
      }
    }
    for (const e of fs.readdirSync(this.path, { withFileTypes: true })) {
      if (e.isDirectory()) {
        try { stopDir(path.join(this.path, e.name)); } catch { /* not a server */ }
      }
    }
    fs.rmSync(this.path, { recursive: true, force: true });
  }
}

async function kill9(child: ChildProcess): Promise<void> {
  const exited = new Promise((r) => child.once('exit', r));
  child.kill('SIGKILL');
  await exited;
}

describe('no leaked servers', () => {
  it('kill -9 of the owner stops the server and removes the dir', async () => {
    const root = new Root('kill9');
    try {
      const owner = await root.owner(false);
      const pm = postmasterPid(owner.dir);
      assert.ok(alive(pm), 'server should be running');
      assert.ok(cmdline(pm).includes(owner.dir), 'postmaster pid names our server');
      assert.equal(ownerJson(owner.dir).owner_pid, owner.child.pid, 'the node probe is the owner');

      await kill9(owner.child);

      const gone = await waitUntil(15_000, () => !alive(pm) && !fs.existsSync(owner.dir));
      assert.ok(gone, `after kill -9: server alive=${alive(pm)}, dir exists=${fs.existsSync(owner.dir)}`);
    } finally {
      root.cleanup();
    }
  });

  it('stop() stops the server, removes the dir and releases the watchdog', async () => {
    const root = new Root('stop');
    try {
      const epg = EmbeddedPg.start({ tempRoot: root.path });
      const dir = epg.dataDir;
      assert.ok(dir.startsWith(root.path + '/'), 'auto dir is under the temp root');
      const pm = postmasterPid(dir);
      const watcher = ownerJson(dir).watcher_pid as number;
      assert.ok(alive(pm) && alive(watcher));
      assert.equal(ownerJson(dir).owner_pid, process.pid);
      assert.equal(epg.isDurable, false);

      epg.stop();

      assert.ok(!alive(pm), 'server still running after stop()');
      assert.ok(!fs.existsSync(dir), 'dir still there after stop()');
      assert.ok(await waitUntil(3_000, () => !alive(watcher)), 'watchdog still running');
      assert.throws(() => epg.port, /stopped/);
    } finally {
      root.cleanup();
    }
  });

  it('sweep reclaims only dead owners\' servers', async () => {
    const root = new Root('sweep');
    let live: EmbeddedPg | undefined;
    try {
      // A: owner alive (this process).
      live = EmbeddedPg.start({ tempRoot: root.path });
      const liveDir = live.dataDir;

      // B: owner AND its watchdog dead — the case only the sweep can clean up.
      // C: durable, owner dead — must survive, and the sweep must leave it.
      // Start both before either dies: every start sweeps.
      const dead = await root.owner(false);
      const durable = await root.owner(true);

      const deadPm = postmasterPid(dead.dir);
      const deadWatcher = ownerJson(dead.dir).watcher_pid as number;
      assert.ok(deadWatcher > 0 && cmdline(deadWatcher).includes('rustypglite-watchdog'));
      // Freeze B's watchdog first so it cannot act, then kill both.
      root.froze(deadWatcher, dead.dir);
      process.kill(deadWatcher, 'SIGSTOP');
      await kill9(dead.child);
      process.kill(deadWatcher, 'SIGKILL');
      assert.ok(await waitUntil(5_000, () => !alive(deadWatcher)));
      assert.ok(alive(deadPm), 'B\'s server should be orphaned, not stopped');

      const durablePm = postmasterPid(durable.dir);
      assert.equal(ownerJson(durable.dir).durable, true);
      assert.equal(ownerJson(durable.dir).watcher_pid, 0, 'no watchdog for a durable server');
      await kill9(durable.child);

      // D: legacy — no owner.json. 0700, as mkdtemp makes them: the sweep
      // will not trust a dir others can write (and umask may be 002).
      const legacy = path.join(root.path, 'rpgl_legacy');
      fs.mkdirSync(legacy);
      fs.chmodSync(legacy, 0o700);
      fs.writeFileSync(path.join(legacy, 'PG_VERSION'), '17\n');

      await sleep(500);
      assert.ok(alive(durablePm), 'a durable server must outlive its owner');

      const report = EmbeddedPg.sweep(root.path);
      assert.ok(report, 'not busy');
      console.log(`  ${JSON.stringify(report)}`);
      assert.deepEqual(report, {
        // C (durable) is rpgldur_*: outside the sweep altogether.
        examined: 3, reclaimed: 1, live: 1, durable: 0, legacy: 1, skipped: 0, failed: 0,
      });

      assert.ok(!alive(deadPm) && !fs.existsSync(dead.dir), 'B reclaimed');
      assert.ok(alive(postmasterPid(liveDir)) && fs.existsSync(liveDir), 'A untouched');
      assert.ok(alive(durablePm) && fs.existsSync(durable.dir), 'C untouched');
      assert.ok(fs.existsSync(path.join(legacy, 'PG_VERSION')), 'D untouched');

      // The explicit stop is the way to end a durable server.
      stopDir(durable.dir);
      assert.ok(!alive(durablePm), 'durable server stopped explicitly');
      assert.ok(!fs.existsSync(durable.dir), 'its auto dir removed');
    } finally {
      live?.stop();
      root.cleanup();
    }
  });

  it('disposing a durable handle detaches; stop() ends it', () => {
    const root = new Root('durable');
    try {
      const a = EmbeddedPg.start({ tempRoot: root.path, durable: true });
      assert.equal(a.isDurable, true);
      const dirA = a.dataDir;
      const pmA = postmasterPid(dirA);
      a[Symbol.dispose]();
      assert.ok(alive(pmA), 'disposing a durable handle must not stop it');

      // A second durable server, stopped through its handle: stop() ends it.
      const b = EmbeddedPg.start({ tempRoot: root.path, durable: true });
      const pmB = postmasterPid(b.dataDir);
      b.stop();
      assert.ok(!alive(pmB), 'stop() ends a durable server');

      // And stopDir ends the detached one.
      EmbeddedPg.stopDir(dirA);
      assert.ok(!alive(pmA), 'stopDir ends a durable server');
      assert.ok(!fs.existsSync(dirA));
      assert.throws(() => stopDir(dirA), /not a data dir/);
    } finally {
      root.cleanup();
    }
  });

  it('detach() leaves a non-durable server running', () => {
    const root = new Root('detach');
    try {
      const epg = EmbeddedPg.start({ tempRoot: root.path });
      const dir = epg.dataDir;
      const pm = postmasterPid(dir);
      epg.detach();
      assert.ok(alive(pm), 'detach must not stop the server');
      stopDir(dir);
      assert.ok(!alive(pm));
    } finally {
      root.cleanup();
    }
  });

  it('sweep returns a report on an empty root and throws on a missing one', () => {
    const root = new Root('empty');
    try {
      assert.deepEqual(sweep(root.path), {
        examined: 0, reclaimed: 0, live: 0, durable: 0, legacy: 0, skipped: 0, failed: 0,
      });
      assert.throws(() => sweep(path.join(root.path, 'nope')), /could not sweep/);
    } finally {
      root.cleanup();
    }
  });

  it('the rustypglite CLI stops a durable server and sweeps', () => {
    const root = new Root('cli');
    try {
      const epg = EmbeddedPg.start({ tempRoot: root.path, durable: true });
      const dir = epg.dataDir;
      const pm = postmasterPid(dir);
      epg.detach();

      const run = (...args: string[]) =>
        spawnSync(process.execPath, [...loader, cli, ...args], { encoding: 'utf8' });

      const stop = run('stop', dir);
      assert.equal(stop.status, 0, stop.stderr);
      assert.equal(stop.stdout, `stopped ${dir}\n`);
      assert.ok(!alive(pm));

      const sw = run('sweep', '--root', root.path);
      assert.equal(sw.status, 0, sw.stderr);
      assert.equal(sw.stdout,
        'examined 0: reclaimed 0, live 0, durable 0, legacy (no owner.json) 0, skipped 0, failed 0\n');

      const bad = run('stop', dir);
      assert.equal(bad.status, 1);
      assert.match(bad.stderr, /^rustypglite: .* is not a data dir of yours\n$/);

      assert.equal(run('sweep', '--bogus').status, 2);
      assert.equal(run().status, 2);
    } finally {
      root.cleanup();
    }
  });
});
