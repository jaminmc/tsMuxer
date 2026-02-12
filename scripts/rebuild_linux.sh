#!/usr/bin/env bash
rm -rf build
mkdir build
cd build || exit
cmake ../ -G Ninja
ninja
mkdir -p ../bin
cp tsMuxer/tsmuxer ../bin/tsMuxeR
cd .. || exit
rm -rf build
