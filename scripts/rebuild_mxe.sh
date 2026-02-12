#!/usr/bin/env bash
export PATH=/usr/lib/mxe/usr/bin:$PATH
rm -rf build
mkdir build
cd build || exit
export CCACHE_DISABLE=1
export MXE_USE_CCACHE=
x86_64-w64-mingw32.static-cmake ../
make
mkdir -p ../bin
cp tsMuxer/tsmuxer.exe ../bin/tsMuxeR.exe
cd .. || exit
rm -rf build
