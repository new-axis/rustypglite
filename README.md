# RustyPGlite

Embedded PostgreSQL as a library — **real Postgres**, no server process to
manage, no Docker, no WASM.

RustyPGlite starts a genuine `postgres` binary against a private data directory
and hands your process a connection. Because it is real Postgres, everything
works: JSONB, extensions, `gen_random_uuid()`, transactions, multiple databases,
several isolated instances at once. Nothing is emulated, so nothing behaves
"almost like" production.

It exists for test suites that had outgrown the alternatives: in-memory fakes
that diverge from Postgres in exactly the places that matter, and WASM builds
that are slow and awkward to pool.

## Using it

**.NET**

```csharp
using var pg = EmbeddedPg.Start();
using var conn = new NpgsqlConnection(pg.ConnectionString);
```

See [DOTNET-INTEGRATION.md](DOTNET-INTEGRATION.md).

**Node**

```typescript
import { EmbeddedPg } from 'rustypglite';
const pg = EmbeddedPg.start();
```

See [NODEJS-INTEGRATION.md](NODEJS-INTEGRATION.md).

**Rust** — the `rustypglite` crate in this workspace.

Design and benchmarks: [ARCHITECTURE.md](ARCHITECTURE.md).

## Platforms

| Platform | Status |
|---|---|
| linux-x64 | Supported. The prebuilt shim is committed, so consumers build with no Rust toolchain. |
| osx-arm64 / osx-x64 | Builds; **not yet verified on real hardware** — see below. |
| linux-arm64 | Should build; unverified. |
| Windows | Not supported. The shim is POSIX (`fork`/`execv`/`dlopen`). |

The PostgreSQL binaries themselves are **not** committed — `scripts/bundle.sh`
downloads them from [zonky's embedded-postgres-binaries][zonky] at bundle time.

### macOS

The macOS port is complete in source but has not been run on Apple hardware at
the time of writing. It is honest to say so: three of the four things that made
this crate Linux-only failed *silently* rather than loudly, and a successful
compile is not evidence that a `fork`-based process manager behaves under
Apple's stricter fork-safety rules. The CI workflow in `.github/workflows/`
exists to answer that question rather than assume it.

To build and bundle locally:

```bash
cargo build --release
scripts/bundle.sh          # writes runtimes/<rid>/native/ where the csproj packs from
```

`bundle.sh` also strips Gatekeeper quarantine from the downloaded Postgres
binaries and ad-hoc signs the shim — both are required for anything headless to
run on Apple Silicon.

## Licence

Dual licensed under either of

* MIT ([LICENSE-MIT](LICENSE-MIT))
* Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE))

at your option.

[zonky]: https://github.com/zonkyio/embedded-postgres-binaries
