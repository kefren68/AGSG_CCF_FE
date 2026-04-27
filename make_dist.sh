#!/bin/bash
# Package source distribution for AGSG CCF FE
# Usage: ./make_dist.sh

set -e

VERSION="2026.04"
DIST_NAME="agsg-ccf-fe-src-${VERSION}"
DIST_DIR="/tmp/${DIST_NAME}"

echo "📦 Packaging source distribution: ${DIST_NAME}"

# Clean previous
rm -rf "$DIST_DIR" "/tmp/${DIST_NAME}.tar.gz"

# Create structure
mkdir -p "$DIST_DIR"

# Copy source files
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cp "$SRC_DIR/launcher.cpp"        "$DIST_DIR/"
cp "$SRC_DIR/Dockerfile"          "$DIST_DIR/"
cp "$SRC_DIR/build.sh"            "$DIST_DIR/"
cp "$SRC_DIR/build_strict.sh"     "$DIST_DIR/"
cp "$SRC_DIR/extensions_cfg.txt"  "$DIST_DIR/"
cp "$SRC_DIR/launcher.sh"         "$DIST_DIR/"
cp "$SRC_DIR/start_local_sd.sh"   "$DIST_DIR/"
cp "$SRC_DIR/INSTALL.txt"         "$DIST_DIR/"
cp "$SRC_DIR/README.md"           "$DIST_DIR/"

# Optional: example gamelist
if [ -f "$SRC_DIR/gamelist.xml" ]; then
    mkdir -p "$DIST_DIR/examples"
    cp "$SRC_DIR/gamelist.xml" "$DIST_DIR/examples/gamelist.xml.example"
fi

# Make scripts executable
chmod +x "$DIST_DIR/build.sh" "$DIST_DIR/build_strict.sh" "$DIST_DIR/launcher.sh" "$DIST_DIR/start_local_sd.sh"

# Create archive
cd /tmp
tar czf "${DIST_NAME}.tar.gz" "${DIST_NAME}"

# Copy archive to project dir
cp "/tmp/${DIST_NAME}.tar.gz" "$SRC_DIR/"

# Cleanup
rm -rf "$DIST_DIR"

echo ""
echo "✅ Distribution package created:"
echo "   📁 ${DIST_NAME}.tar.gz ($(ls -lh "$SRC_DIR/${DIST_NAME}.tar.gz" | awk '{print $5}'))"
echo ""
echo "Contents:"
tar tzf "$SRC_DIR/${DIST_NAME}.tar.gz" | sed 's/^/   /'
