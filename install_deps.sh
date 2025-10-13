#!/bin/bash
# Install dependencies for Luanti with tracking export

echo "Installing development packages for Luanti build..."

sudo apt-get update

sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    libfreetype-dev \
    libcurl4-openssl-dev \
    libsdl2-dev \
    libjsoncpp-dev \
    zlib1g-dev \
    libzstd-dev \
    libsqlite3-dev \
    libgmp-dev \
    libpng-dev \
    libjpeg-dev

echo ""
echo "✓ Dependencies installed!"
echo ""
echo "Now run:"
echo "  cd /home/robert/CodeProjects/luanti-tracking/build"
echo "  cmake .. -DENABLE_TRACKING_EXPORT=ON -DBUILD_SERVER=OFF -DENABLE_SOUND=OFF -DENABLE_GETTEXT=OFF"
echo "  make -j\$(nproc)"
