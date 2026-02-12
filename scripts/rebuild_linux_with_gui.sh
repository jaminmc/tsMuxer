#!/usr/bin/env bash
rm -rf build
mkdir build
cd build || exit
cmake ../ -G Ninja -DTSMUXER_GUI=ON
ninja
