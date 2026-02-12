#!/usr/bin/env bash
set -euo pipefail
rm -rf build
mkdir build
cd build || exit
cmake -DTSMUXER_STATIC_BUILD=ON -DFREETYPE_LDFLAGS="bz2;brotlidec;brotlicommon;png" ../
make
cp tsMuxer/tsmuxer ../bin/tsMuxeR
cd .. || exit
rm -rf build
ls ./bin/tsMuxeR
