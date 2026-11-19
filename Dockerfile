# Dockerfile for gamestation-sdk-armhf with ffmpeg 4.3 (libavformat 58)
FROM debian:bullseye

RUN echo "deb http://archive.debian.org/debian bullseye main" > /etc/apt/sources.list && \
    echo "deb http://archive.debian.org/debian-security bullseye-security main" >> /etc/apt/sources.list && \
    echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99no-check-valid-until && \
    dpkg --add-architecture armhf && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
      build-essential \
      gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf \
      libsdl2-dev:armhf libsdl2-image-dev:armhf libsdl2-ttf-dev:armhf \
      libavformat-dev:armhf libavcodec-dev:armhf libavutil-dev:armhf libswscale-dev:armhf libswresample-dev:armhf \
      libfreetype6-dev:armhf libpng-dev:armhf zlib1g-dev:armhf \
      pkg-config:armhf \
      sudo \
      git \
      wget \
      ca-certificates \
      && apt-get clean && rm -rf /var/lib/apt/lists/*

# Set up user and workdir
WORKDIR /build
