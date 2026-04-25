#!/usr/bin/env bash
set -euo pipefail
export PATH=/usr/lib/mxe/usr/bin:$PATH
# Unset LD_LIBRARY_PATH so MXE host tools (uic, moc, rcc) use their own Qt
# libs instead of picking up an incompatible /opt/qt install.
unset LD_LIBRARY_PATH
rm -rf build
mkdir build
mkdir -p ./bin/w32-qt5
cd build || exit
export CCACHE_DISABLE=1
export MXE_USE_CCACHE=

# Build with Qt5 for Windows 7 compatibility (32-bit)
i686-w64-mingw32.static-cmake ../ -DTSMUXER_GUI=ON -DQt5_DIR=/usr/lib/mxe/usr/i686-w64-mingw32.static/qt5
make
mv tsMuxer/tsmuxer.exe ../bin/w32-qt5/tsMuxeR.exe
mv tsMuxerGUI/tsMuxerGUI.exe ../bin/w32-qt5/tsMuxerGUI.exe
cd .. || exit
rm -rf build
zip -jr ./bin/w32-qt5.zip ./bin/w32-qt5
ls ./bin/w32-qt5/tsMuxeR.exe && ls ./bin/w32-qt5/tsMuxerGUI.exe && ls ./bin/w32-qt5.zip
