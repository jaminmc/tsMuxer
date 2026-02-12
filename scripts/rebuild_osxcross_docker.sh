#!/usr/bin/env bash
#
# Build universal (arm64 + x86_64) macOS CLI binary using osxcross.
# Designed to run inside the jaminmc/tsmuxer_build Docker container.
#

set -ex

export PATH=/usr/lib/osxcross/bin:$PATH

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/osxcross_common.sh"

ARM64_CMAKE=$(find_tool arm64 cmake)
X86_64_CMAKE=$(find_tool x86_64 cmake)
LIPO=$(find_tool x86_64 lipo)

# ---------------------------------------------------------------------------
# Common CMake arguments
# ---------------------------------------------------------------------------
CMAKE_COMMON_ARGS=(
  -DTSMUXER_STATIC_BUILD=ON
  -DCMAKE_TOOLCHAIN_FILE="${OSXCROSS_ROOT}/toolchain.cmake"
  -DWITHOUT_PKGCONFIG=TRUE
  -DFREETYPE_LIBRARY="${FREETYPE_PREFIX}/lib/libfreetype.a"
  -DFREETYPE_INCLUDE_DIRS="${FREETYPE_PREFIX}/include"
)

# ---------------------------------------------------------------------------
# Build for arm64
# ---------------------------------------------------------------------------
rm -rf build-arm64 build-x86_64
mkdir build-arm64
cd build-arm64 || exit
"$ARM64_CMAKE" ../ "${CMAKE_COMMON_ARGS[@]}"
make -j"$(nproc)"
cd .. || exit

# ---------------------------------------------------------------------------
# Build for x86_64
# ---------------------------------------------------------------------------
mkdir build-x86_64
cd build-x86_64 || exit
"$X86_64_CMAKE" ../ "${CMAKE_COMMON_ARGS[@]}"
make -j"$(nproc)"
cd .. || exit

# ---------------------------------------------------------------------------
# Create universal binary with lipo
# ---------------------------------------------------------------------------
mkdir -p bin
"$LIPO" -create \
  build-arm64/tsMuxer/tsmuxer \
  build-x86_64/tsMuxer/tsmuxer \
  -output bin/tsMuxeR

rm -rf build-arm64 build-x86_64
ls -l bin/tsMuxeR
