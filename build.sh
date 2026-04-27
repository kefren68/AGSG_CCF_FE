#!/bin/bash

# Auto-compilation script for Gamestation Go Launcher
# Usage: ./build.sh [clean]

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCKER_IMAGE="gamestation-sdk-armhf:latest"
SOURCE_FILE="launcher.cpp"
OUTPUT_BINARY="launcher"

echo "🔨 Building Gamestation Go Launcher..."
echo "📁 Project: $PROJECT_DIR"

# Check if Docker image exists
if ! sudo docker image inspect "$DOCKER_IMAGE" > /dev/null 2>&1; then
    echo "❌ Error: Docker image '$DOCKER_IMAGE' not found!"
    echo "💡 Build the image with: docker build -t gamestation-sdk-armhf ."
    exit 1
fi

# Build
sudo docker run --rm -v "$PROJECT_DIR":/build "$DOCKER_IMAGE" \
  bash -c "cd /build && \
    arm-linux-gnueabihf-g++ $SOURCE_FILE -o $OUTPUT_BINARY \
    -march=armv7-a -mfloat-abi=hard -mfpu=neon \
    -I/usr/include/arm-linux-gnueabihf/SDL2 \
    -L/usr/lib/arm-linux-gnueabihf \
    -lSDL2 -lSDL2_image -ldl \
    -lavformat -lavcodec -lswresample -lswscale -lavutil \
    -lm -lz -lpthread \
    -O3 -ffast-math -flto -fomit-frame-pointer -funroll-loops \
    -Wall -Wextra -std=c++11" \
  2>&1 | tee build.log

# Verify binary
if [ -f "$PROJECT_DIR/$OUTPUT_BINARY" ]; then
    SIZE=$(ls -lh "$PROJECT_DIR/$OUTPUT_BINARY" | awk '{print $5}')
    ARCH=$(file "$PROJECT_DIR/$OUTPUT_BINARY" | grep -o "ARM" || echo "ERROR")
    
    echo ""
    echo "✅ Build successful!"
    echo "📦 Binary: $OUTPUT_BINARY ($SIZE)"
    echo "🎯 Architecture: $ARCH EABI5 NEON"
    echo "⚡ Optimizations: O3 LTO ffast-math neon32"
    echo ""
    echo "🖥️  Building native preview binary..."
    bash "$PROJECT_DIR/build_native.sh"
    exit 0
else
    echo ""
    echo "❌ Build failed! Binary not found."
    exit 1
fi