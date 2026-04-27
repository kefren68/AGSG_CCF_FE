#!/bin/bash

# Native x86_64 build for local theme preview on Linux
# Usage: ./build_native.sh

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_FILE="launcher.cpp"
OUTPUT_BINARY="launcher_native"

echo "🔨 Building launcher (native x86_64 for theme preview)..."
echo "📁 Project: $PROJECT_DIR"

# Check and install missing dependencies
MISSING_PKGS=()
dpkg -s libavformat-dev &>/dev/null  || MISSING_PKGS+=(libavformat-dev)
dpkg -s libavcodec-dev  &>/dev/null  || MISSING_PKGS+=(libavcodec-dev)
dpkg -s libavutil-dev   &>/dev/null  || MISSING_PKGS+=(libavutil-dev)
dpkg -s libswscale-dev  &>/dev/null  || MISSING_PKGS+=(libswscale-dev)
dpkg -s libswresample-dev &>/dev/null || MISSING_PKGS+=(libswresample-dev)

if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
    echo "📦 Installing missing packages: ${MISSING_PKGS[*]}"
    sudo apt-get install -y "${MISSING_PKGS[@]}"
fi

# Compile natively, pointing base_p to the project directory
g++ "$PROJECT_DIR/$SOURCE_FILE" -o "$PROJECT_DIR/$OUTPUT_BINARY" \
    -DNATIVE_BASE_PATH \
    $(sdl2-config --cflags --libs) \
    -lSDL2_image \
    -lavformat -lavcodec -lswresample -lswscale -lavutil \
    -lm -lz -lpthread -ldl \
    -O2 -std=c++11 \
    -Wall -Wextra \
    2>&1

if [ -f "$PROJECT_DIR/$OUTPUT_BINARY" ]; then
    SIZE=$(ls -lh "$PROJECT_DIR/$OUTPUT_BINARY" | awk '{print $5}')
    echo ""
    echo "✅ Build successful!"
    echo "📦 Binary: $OUTPUT_BINARY ($SIZE)"
    echo "🖥️  Architecture: x86_64 (native)"
    echo ""
    echo "▶️  Run with: ./$OUTPUT_BINARY"
    echo "   (themes/ e images/ vengono letti da: $PROJECT_DIR/)"
else
    echo "❌ Build failed!"
    exit 1
fi
