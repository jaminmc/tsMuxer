#!/usr/bin/env bash
export PATH=/usr/lib/mxe/usr/bin:$PATH
# Unset LD_LIBRARY_PATH so MXE host tools (uic, moc, rcc) use their own Qt
# libs instead of picking up an incompatible /opt/qt install.
unset LD_LIBRARY_PATH
rm -rf build
mkdir build
cd build || exit
export CCACHE_DISABLE=1
export MXE_USE_CCACHE=
x86_64-w64-mingw32.static-cmake ../ -DTSMUXER_GUI=ON
make
cp tsMuxer/tsmuxer.exe ../bin/tsMuxeR.exe
cp tsMuxer/tsmuxergui.exe ../bin/tsMuxerGUI.exe
cd .. || exit
rm -rf build
