#!/usr/bin/env node
// `rustypglite` — stop a server by its data dir, or sweep for dead owners' servers.
//
//   rustypglite stop <data-dir>...     stop exactly these servers (e.g. from `run.sh down`)
//   rustypglite sweep [--root <dir>]   reclaim servers whose owner is dead, and report
//
// Same behaviour and output as the Rust CLI (rustypglite/src/bin/rustypglite.rs).
// Needs no Postgres binaries: stopping is by signal to the postmaster named in
// the dir's postmaster.pid.

import { stopDir, sweep } from './index.js';

const USAGE = `usage:
  rustypglite stop <data-dir>...     stop the server in each dir; remove the dir
                                     only if rustypglite created it
  rustypglite sweep [--root <dir>]   stop and remove servers whose owner is dead
                                     (default root: $RUSTYPGLITE_TMPDIR, else /tmp)`;

function message(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

function main(args: string[]): number {
  const cmd = args[0];
  if (cmd === 'stop' && args.length > 1) {
    let ok = true;
    for (const dir of args.slice(1)) {
      try {
        stopDir(dir);
        console.log(`stopped ${dir}`);
      } catch (e) {
        console.error(`rustypglite: ${message(e)}`);
        ok = false;
      }
    }
    return ok ? 0 : 1;
  }
  if (cmd === 'sweep') {
    const rest = args.slice(1);
    let root: string | undefined;
    if (rest.length === 0) {
      root = undefined;
    } else if (rest.length === 2 && rest[0] === '--root') {
      root = rest[1];
    } else {
      console.error(USAGE);
      return 2;
    }
    try {
      const r = sweep(root);
      if (r === null) {
        console.log('another process is sweeping this root right now');
        return 0;
      }
      console.log(
        `examined ${r.examined}: reclaimed ${r.reclaimed}, live ${r.live}, durable ${r.durable}, ` +
        `legacy (no owner.json) ${r.legacy}, skipped ${r.skipped}, failed ${r.failed}`
      );
      return r.failed > 0 ? 1 : 0;
    } catch (e) {
      console.error(`rustypglite: ${message(e)}`);
      return 1;
    }
  }
  console.error(USAGE);
  return 2;
}

process.exitCode = main(process.argv.slice(2));
