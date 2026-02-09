#!/usr/bin/bash

cd ~
if [ "$MSYSTEM" == "MSYS" ] ; then
 pacman -Syu
 pacman -Sy --needed base-devel \
 flex \
 zlib-devel \
 git
 if [ ! -d tsmuxer ] ; then
  git clone https://github.com/jaminmc/tsMuxer.git
 fi
else
 cd tsmuxer
 if [ ! -d build ] ; then
  pacman -Sy --needed $MINGW_PACKAGE_PREFIX-toolchain \
  $MINGW_PACKAGE_PREFIX-cmake \
  $MINGW_PACKAGE_PREFIX-freetype \
  $MINGW_PACKAGE_PREFIX-zlib \
  $MINGW_PACKAGE_PREFIX-ninja \
  $MINGW_PACKAGE_PREFIX-qt6-static
  mkdir build
 fi
 git pull
 cd build
 cmake ../ -G Ninja -DTSMUXER_STATIC_BUILD=true -DTSMUXER_GUI=ON
 ninja && cp -u tsMuxer/tsmuxer.exe ../bin/
 if [ -f tsMuxerGUI/tsMuxerGUI.exe ] ; then
  cp -u tsMuxerGUI/tsMuxerGUI.exe ../bin/
 fi
 cd ..
fi
