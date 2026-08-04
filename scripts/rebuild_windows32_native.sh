#!/usr/bin/env bash
set -euo pipefail

# Native Windows 32-bit Win7 GUI build (MSVC v142 + Qt 5.15).
# Requires Qt 5.15 win32_msvc2019 on CMAKE_PREFIX_PATH / Qt5_DIR,
# and the VS 2019 (v142) toolset for a Win7-compatible CRT.

rm -rf build
mkdir build
cd build || exit

cmake ../ \
  -A Win32 \
  -T v142 \
  -DTSMUXER_GUI=ON \
  -DTSMUXER_WIN7=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DWITHOUT_PKGCONFIG=TRUE
cmake --build . --config Release

mkdir -p ../bin
cp tsMuxer/Release/tsmuxer.exe ../bin/tsMuxeR.exe
if [ -f tsMuxerGUI/Release/tsmuxergui.exe ]; then
  cp tsMuxerGUI/Release/tsmuxergui.exe ../bin/tsMuxerGUI.exe
fi
cd .. || exit

echo "Build complete: bin/tsMuxeR.exe"
if [ -f bin/tsMuxerGUI.exe ]; then
  echo "GUI build complete: bin/tsMuxerGUI.exe (Win7 / Qt5.15)"
fi
