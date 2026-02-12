#!/usr/bin/env bash
rm -rf build
mkdir build
cd build || exit
cmake ../ -G Ninja
ninja
cp tsMuxer/tsmuxer ../bin/tsMuxeR
cd .. || exit
rm -rf build
