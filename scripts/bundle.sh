#!/usr/bin/env bash
set -euo pipefail

# bundle.sh — Download PG binaries + package with native lib
#
# Creates: rustypglite-csharp/RustyPGlite/runtimes/<rid>/native/
#   ├── librustypglite.so (or .dylib)
#   └── pg/ (downloaded from zonkyio/embedded-postgres-binaries)
#
# TWO THINGS THIS SCRIPT GETS RIGHT THAT IT USED NOT TO, both of which failed
# silently rather than loudly:
#
#  1. THE DIRECTORY IS A .NET RUNTIME IDENTIFIER (osx-arm64), not a uname-shaped
#     name (darwin-arm64). NuGet resolves native assets by RID and by nothing
#     else, so the old name produced a package whose dylib was present, correct,
#     and never loaded — surfacing much later as "unable to load shared library".
#
#  2. IT WRITES WHERE THE CSPROJ READS. Output went to dist/, the csproj packs
#     from runtimes/, and a human was expected to move it. An un-run copy step
#     looks exactly like a build that produced nothing.
#
# Override the destination with RUSTYPGLITE_BUNDLE_DIR if you are staging a
# multi-RID release from several machines.

PG_VERSION="${RUSTYPGLITE_PG_VERSION:-17.5.0}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

# Detect platform. RID is what .NET/NuGet match on; ZIO_* select the zonky
# embedded-postgres artifact, which uses its own OS/arch vocabulary.
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)   RID="linux-x64";   EXT="so";    ZIO_OS="linux";  ZIO_ARCH="amd64" ;;
    Linux-aarch64)  RID="linux-arm64"; EXT="so";    ZIO_OS="linux";  ZIO_ARCH="arm64v8" ;;
    Darwin-arm64)   RID="osx-arm64";   EXT="dylib"; ZIO_OS="darwin"; ZIO_ARCH="arm64v8" ;;
    Darwin-x86_64)  RID="osx-x64";     EXT="dylib"; ZIO_OS="darwin"; ZIO_ARCH="amd64" ;;
    *)              echo "Unsupported: $(uname -s)-$(uname -m)"; exit 1 ;;
esac

# Alpine links against musl rather than glibc, so a glibc build of the shim will
# not load there. Refuse rather than produce a bundle that fails at dlopen time.
if [ "$ZIO_OS" = "linux" ] && [ -f /etc/alpine-release ]; then
    echo "bundle.sh: Alpine/musl is not a supported bundle host (the shim is built against glibc)" >&2
    exit 1
fi

NATIVE_LIB="$ROOT_DIR/target/release/librustypglite.$EXT"
if [ ! -f "$NATIVE_LIB" ]; then
    echo "Run: cargo build --release"
    exit 1
fi

BUNDLE_ROOT="${RUSTYPGLITE_BUNDLE_DIR:-$ROOT_DIR/rustypglite-csharp/RustyPGlite/runtimes}"
OUT_DIR="$BUNDLE_ROOT/$RID/native"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

echo "Bundling for $RID (PG $PG_VERSION)..."

# 1. Copy native library
cp "$NATIVE_LIB" "$OUT_DIR/"

# 2. Download PG binaries
ARTIFACT="embedded-postgres-binaries-${ZIO_OS}-${ZIO_ARCH}"
URL="https://repo1.maven.org/maven2/io/zonky/test/postgres/${ARTIFACT}/${PG_VERSION}/${ARTIFACT}-${PG_VERSION}.jar"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "  Downloading PG from Maven Central..."
curl -fsSL "$URL" -o "$TMPDIR/pg.jar"
(cd "$TMPDIR" && unzip -q pg.jar "*.txz")
mkdir -p "$OUT_DIR/pg"
tar xJf "$TMPDIR"/*.txz -C "$OUT_DIR/pg"

# macOS quarantines anything that arrived over the network, and the quarantine
# attribute is INHERITED by files extracted from a downloaded archive. Gatekeeper
# then refuses to execute initdb/postgres with a dialog no headless test run can
# answer — it presents as a hang or an opaque spawn failure. Strip it here, at
# the one place that knows the bytes came from Maven Central.
if [ "$ZIO_OS" = "darwin" ]; then
    xattr -dr com.apple.quarantine "$OUT_DIR" 2>/dev/null || true
    # An ad-hoc signature is enough for a locally built dylib to load on Apple
    # Silicon; without one, hardened-runtime hosts reject it.
    codesign --force --sign - "$OUT_DIR/librustypglite.$EXT" 2>/dev/null \
        || echo "  note: codesign unavailable — install Xcode CLT if loading fails"
fi

echo ""
echo "Bundle: $OUT_DIR"
du -sh "$OUT_DIR"
du -sh "$OUT_DIR/librustypglite.$EXT"
du -sh "$OUT_DIR/pg/"
echo "$("$OUT_DIR/pg/bin/postgres" --version)"
