#!/usr/bin/env bash
set -euo pipefail
rm -rf build
mkdir build
cd build || exit
cmake ../ -G Ninja -DTSMUXER_GUI=ON
ninja

mkdir -p ../bin/lnx
cp tsMuxer/tsmuxer ../bin/lnx/tsMuxeR
cp tsMuxerGUI/tsMuxerGUI ../bin/lnx/tsMuxerGUI
cd .. || exit

rm -f bin/lnx.zip
(cd bin && zip -r lnx.zip lnx)
ls bin/lnx/tsMuxeR bin/lnx/tsMuxerGUI bin/lnx.zip
