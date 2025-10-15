#!/bin/bash
set -e

echo "============================================"
echo "Building Image Compressor Server"
echo "============================================"

echo "Step 1: Compiling C++ compression code..."
g++ -O3 compress.cpp lodepng.cpp -o compress -static

echo "Step 2: Verifying compiled binary..."
ls -lh compress || echo "Binary not found!"
file compress || echo "Cannot determine file type"

echo "Step 3: Making binary executable..."
chmod +x compress

echo "Step 4: Installing Node.js dependencies..."
npm install --production

echo "============================================"
echo "Build completed successfully!"
echo "============================================"