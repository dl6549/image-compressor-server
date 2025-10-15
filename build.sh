#!/bin/bash
set -e

echo "============================================"
echo "Building Image Compressor Server"
echo "============================================"

echo "Step 1: Installing build dependencies..."
apt-get update -qq
apt-get install -y g++ make

echo "Step 2: Compiling C++ compression code..."
g++ -O3 compress.cpp lodepng.cpp -o compress -static

echo "Step 3: Verifying compiled binary..."
ls -lh compress
file compress

echo "Step 4: Installing Node.js dependencies..."
npm install --production

echo "============================================"
echo "Build completed successfully!"
echo "============================================"
```