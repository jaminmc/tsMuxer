#!/usr/bin/env bash
set -euo pipefail

# Native Windows x64 build (MSVC + Qt6). Intended for Git Bash / developer machines
# with Qt6 on CMAKE_PREFIX_PATH, or CI via jurplel/install-qt-action.

rm -rf build
mkdir build
cd build || exit

cmake ../ \
  -DTSMUXER_GUI=ON \
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
  echo "GUI build complete: bin/tsMuxerGUI.exe"
fi
