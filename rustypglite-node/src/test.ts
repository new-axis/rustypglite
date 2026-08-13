import { describe, it, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { EmbeddedPg } from './index.js';
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
