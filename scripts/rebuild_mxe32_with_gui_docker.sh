#!/usr/bin/env bash
export PATH=/usr/lib/mxe/usr/bin:$PATH
# Unset LD_LIBRARY_PATH so MXE host tools (uic, moc, rcc) use their own Qt
# libs instead of picking up an incompatible /opt/qt install.
unset LD_LIBRARY_PATH
rm -rf build
mkdir build
mkdir -p ./bin/w32
cd build || exit
export CCACHE_DISABLE=1
export MXE_USE_CCACHE=

i686-w64-mingw32.static-cmake ../ -DTSMUXER_GUI=ON
make
mv tsMuxer/tsmuxer.exe ../bin/w32/tsMuxeR.exe
mv tsMuxerGUI/tsMuxerGUI.exe ../bin/w32/tsMuxerGUI.exe
cd .. || exit
rm -rf build
zip -jr ./bin/w32.zip ./bin/w32
ls ./bin/w32/tsMuxeR.exe && ls ./bin/w32/tsMuxerGUI.exe && ls ./bin/w32.zip
