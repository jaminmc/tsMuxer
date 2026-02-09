#!/usr/bin/env bash

set -e

mkdir build
cd build
cmake -DTSMUXER_GUI=TRUE ..
make -j"$(nproc --all)" tsMuxerGUI
