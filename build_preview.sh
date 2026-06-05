#!/bin/bash

# Cross-platform preview build (Linux x86_64)
# Produces launcher_preview - same as launcher_native but without Linux-specific
# features (wifi, battery, volume keys, dlfcn). Suitable as a base for porting
# to Windows/macOS.
# Usage: ./build_preview.sh

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_FILE="launcher.cpp"
OUTPUT_BINARY="launcher_preview"

echo "🔨 Building launcher_preview (cross-platform build)..."
echo "📁 Project: $PROJECT_DIR"

# Check and install missing dependencies
MISSING_PKGS=()
dpkg -s libavformat-dev   &>/dev/null || MISSING_PKGS+=(libavformat-dev)
dpkg -s libavcodec-dev    &>/dev/null || MISSING_PKGS+=(libavcodec-dev)
dpkg -s libavutil-dev     &>/dev/null || MISSING_PKGS+=(libavutil-dev)
dpkg -s libswscale-dev    &>/dev/null || MISSING_PKGS+=(libswscale-dev)
dpkg -s libswresample-dev &>/dev/null || MISSING_PKGS+=(libswresample-dev)
dpkg -s libsdl2-ttf-dev   &>/dev/null || MISSING_PKGS+=(libsdl2-ttf-dev)

if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
    echo "📦 Installing missing packages: ${MISSING_PKGS[*]}"
    sudo apt-get install -y "${MISSING_PKGS[@]}"
fi

# Compile with CROSS_PLATFORM + NATIVE_BASE_PATH
# -DCROSS_PLATFORM disables: dlopen/TTF dynamic loading, /proc wifi/battery,
#                            /dev/input volume keys, Linux signal handlers
# Links SDL2_ttf directly instead of dlopen at runtime
BUILD_TS="$(date '+%Y-%m-%d %H:%M:%S')"
g++ "$PROJECT_DIR/$SOURCE_FILE" -o "$PROJECT_DIR/$OUTPUT_BINARY" \
    -DNATIVE_BASE_PATH \
    -DCROSS_PLATFORM \
    "-DBUILD_TIMESTAMP=\"$BUILD_TS\"" \
    $(sdl2-config --cflags --libs) \
    -lSDL2_image \
    -lSDL2_ttf \
    -lavformat -lavcodec -lswresample -lswscale -lavutil \
    -lm -lz -lpthread \
    -O2 -std=c++11 \
    -Wall -Wextra \
    2>&1

if [ -f "$PROJECT_DIR/$OUTPUT_BINARY" ]; then
    SIZE=$(ls -lh "$PROJECT_DIR/$OUTPUT_BINARY" | awk '{print $5}')
    echo ""
    echo "✅ Build successful!"
    echo "📦 Binary: $OUTPUT_BINARY ($SIZE)"
    echo "🖥️  Architecture: x86_64 (cross-platform preview)"
    echo ""
    echo "▶️  Run with: ./$OUTPUT_BINARY"
    echo "   (themes/ e images/ vengono letti da: $PROJECT_DIR/)"
else
    echo "❌ Build failed!"
    exit 1
fi
