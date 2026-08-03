#!/usr/bin/env bash
set -euo pipefail

# Native Windows 32-bit CLI build (MSVC). Qt6 has no official 32-bit MSVC packages,
# so the GUI is disabled here.

rm -rf build
mkdir build
cd build || exit

cmake ../ \
  -A Win32 \
  -DTSMUXER_GUI=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DWITHOUT_PKGCONFIG=TRUE
cmake --build . --config Release

mkdir -p ../bin
cp tsMuxer/Release/tsmuxer.exe ../bin/tsMuxeR.exe
cd .. || exit

echo "Build complete: bin/tsMuxeR.exe"
