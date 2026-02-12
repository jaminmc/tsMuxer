#!/usr/bin/env bash
set -euo pipefail
rm -rf build
mkdir build
cd build || exit
cmake ../ -G Ninja -DTSMUXER_GUI=ON
ninja
