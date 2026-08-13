#!/usr/bin/env bash
set -euo pipefail

# download-pg.sh — Download prebuilt PostgreSQL binaries from zonkyio
#
# Downloads the right binary for the current platform and extracts it
# into native/pg/ next to the native library.
#
# Source: https://github.com/zonkyio/embedded-postgres-binaries
# Hosted on Maven Central (no auth required).

PG_VERSION="${RUSTYPGLITE_PG_VERSION:-17.5.0}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_DIR="$(dirname "$SCRIPT_DIR")"
NATIVE_DIR="$PKG_DIR/native"
PG_DIR="$NATIVE_DIR/pg"

# Skip if already downloaded
if [ -f "$PG_DIR/bin/postgres" ]; then
    echo "rustypglite: PostgreSQL $PG_VERSION already installed"
    exit 0
fi

# Detect platform
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"

case "$OS" in
    linux)  OS_NAME="linux" ;;
    darwin) OS_NAME="darwin" ;;
    *)      echo "rustypglite: Unsupported OS: $OS"; exit 1 ;;
esac

case "$ARCH" in
    x86_64)  ARCH_NAME="amd64" ;;
    aarch64|arm64) ARCH_NAME="arm64v8" ;;
    *)       echo "rustypglite: Unsupported architecture: $ARCH"; exit 1 ;;
esac

# Check for Alpine Linux (musl)
if [ "$OS_NAME" = "linux" ] && [ -f /etc/alpine-release ]; then
    ARCH_NAME="${ARCH_NAME}-alpine"
fi

ARTIFACT="embedded-postgres-binaries-${OS_NAME}-${ARCH_NAME}"
URL="https://repo1.maven.org/maven2/io/zonky/test/postgres/${ARTIFACT}/${PG_VERSION}/${ARTIFACT}-${PG_VERSION}.jar"

echo "rustypglite: Downloading PostgreSQL ${PG_VERSION} for ${OS_NAME}-${ARCH_NAME}..."
echo "rustypglite: URL: ${URL}"

# Download
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

JAR_FILE="$TMPDIR/pg.jar"
curl -fsSL "$URL" -o "$JAR_FILE" || {
    echo "rustypglite: Download failed. Check your internet connection."
    echo "rustypglite: URL was: $URL"
    exit 1
}

# Extract: JAR → txz → pg directory
cd "$TMPDIR"
unzip -q "$JAR_FILE" "*.txz"
TXZ_FILE=$(ls *.txz)

mkdir -p "$PG_DIR"
tar xJf "$TXZ_FILE" -C "$PG_DIR"

echo "rustypglite: PostgreSQL ${PG_VERSION} installed to ${PG_DIR}"
echo "rustypglite: $(${PG_DIR}/bin/postgres --version)"
