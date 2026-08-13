#!/usr/bin/env bash
set -euo pipefail

# bootstrap-pg.sh — provision the native PostgreSQL that RustyPGlite runs.
#
# Downloads the prebuilt PostgreSQL binaries for this platform from
# zonkyio/embedded-postgres-binaries (Maven Central, no auth) and extracts
# them into a local install directory. Idempotent: exits 0 immediately if the
# target already contains bin/postgres.
#
# This is the "fresh machine / CI" counterpart of the RUSTYPGLITE_PG_DIR
# convention: run it once, point RUSTYPGLITE_PG_DIR at the target, and any
# consumer (dotnet test, node, rust) has an embedded Postgres.
#
# Usage:
#   bash scripts/bootstrap-pg.sh [target-dir]
#
#   target-dir     defaults to $RUSTYPGLITE_PG_DIR if set,
#                  else $HOME/.cache/rustypglite/pg-install
#   PG version     override with RUSTYPGLITE_PG_VERSION (default 17.5.0)
#
# After it completes:
#   export RUSTYPGLITE_PG_DIR=<target-dir>
#   export LD_LIBRARY_PATH=$RUSTYPGLITE_PG_DIR/lib

PG_VERSION="${RUSTYPGLITE_PG_VERSION:-17.5.0}"
TARGET="${1:-${RUSTYPGLITE_PG_DIR:-$HOME/.cache/rustypglite/pg-install}}"

if [ -x "$TARGET/bin/postgres" ]; then
    echo "bootstrap-pg: already provisioned at $TARGET"
    echo "bootstrap-pg: $("$TARGET/bin/postgres" --version)"
    exit 0
fi

for tool in curl unzip tar; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "bootstrap-pg: missing required tool: $tool" >&2; exit 1; }
done

# Detect platform (same matrix as rustypglite-node/scripts/download-pg.sh)
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"

case "$OS" in
    linux)  OS_NAME="linux" ;;
    darwin) OS_NAME="darwin" ;;
    *)      echo "bootstrap-pg: unsupported OS: $OS" >&2; exit 1 ;;
esac

case "$ARCH" in
    x86_64)        ARCH_NAME="amd64" ;;
    aarch64|arm64) ARCH_NAME="arm64v8" ;;
    *)             echo "bootstrap-pg: unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

# Alpine (musl) variant
if [ "$OS_NAME" = "linux" ] && [ -f /etc/alpine-release ]; then
    ARCH_NAME="${ARCH_NAME}-alpine"
fi

ARTIFACT="embedded-postgres-binaries-${OS_NAME}-${ARCH_NAME}"
URL="https://repo1.maven.org/maven2/io/zonky/test/postgres/${ARTIFACT}/${PG_VERSION}/${ARTIFACT}-${PG_VERSION}.jar"

echo "bootstrap-pg: downloading PostgreSQL ${PG_VERSION} (${OS_NAME}-${ARCH_NAME})"
echo "bootstrap-pg: ${URL}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

curl -fsSL "$URL" -o "$WORK/pg.jar" || {
    echo "bootstrap-pg: download failed (URL above)" >&2; exit 1; }

# JAR → embedded .txz → install tree
(cd "$WORK" && unzip -q pg.jar '*.txz')
TXZ="$(ls "$WORK"/*.txz)"

# Extract to a staging dir first so a failed extract never leaves a
# half-populated target that the idempotency check would then skip.
mkdir -p "$WORK/stage"
tar xJf "$TXZ" -C "$WORK/stage"

mkdir -p "$(dirname "$TARGET")"
rm -rf "$TARGET"
mv "$WORK/stage" "$TARGET"

echo "bootstrap-pg: installed to $TARGET"
echo "bootstrap-pg: $("$TARGET/bin/postgres" --version)"
echo ""
echo "  export RUSTYPGLITE_PG_DIR=$TARGET"
echo "  export LD_LIBRARY_PATH=\$RUSTYPGLITE_PG_DIR/lib"
