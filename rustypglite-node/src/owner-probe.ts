// Test helper: an "owner" process for the no-leaked-servers tests in test.ts.
// Starts a server under the temp root given as argv[2] (durable if argv[3] is
// "durable"), prints RPGL_DIR=<data dir>, then sleeps until killed.
// Run it in-process (`node --import tsx src/owner-probe.ts ...`), not via the
// `tsx` CLI, so the PID the test kills is the process that owns the server.

import { EmbeddedPg } from './index.js';

const root = process.argv[2];
if (!root) {
  console.error('usage: owner-probe <temp-root> [durable]');
  process.exit(2);
}
const pg = EmbeddedPg.start({ tempRoot: root, durable: process.argv[3] === 'durable' });
process.stdout.write(`RPGL_DIR=${pg.dataDir}\n`);
setInterval(() => {}, 1 << 30); // until killed
