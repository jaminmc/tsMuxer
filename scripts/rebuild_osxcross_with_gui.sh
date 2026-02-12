#!/usr/bin/env bash
#
# Build universal (arm64 + x86_64) macOS CLI + GUI binaries using osxcross.
# Standalone version (runs directly inside a machine with osxcross installed).
#
# Qt6 for macOS is installed via aqtinstall and is already universal.
# Freetype is built as a universal static library.
#

set -exo pipefail

export PATH=/usr/lib/osxcross/bin:$PATH

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/osxcross_common.sh"

# QT_MAC_X64 is set by the Docker image's ENV; fall back to default path
readonly QT_MAC=${QT_MAC_X64:-/opt/qt-mac/6.8.2/macos}
# Host (Linux) Qt is needed for moc/uic/rcc during cross-compilation
readonly QT_HOST=${QT_HOST_PATH:-/opt/qt/6.8.2/gcc_64}

ARM64_CMAKE=$(find_tool arm64 cmake)
X86_64_CMAKE=$(find_tool x86_64 cmake)
LIPO=$(find_tool x86_64 lipo)
OTOOL=$(find_tool x86_64 otool)
INSTALL_NAME_TOOL=$(find_tool x86_64 install_name_tool)

# ---------------------------------------------------------------------------
# Create a wrapper toolchain that includes osxcross + adds Qt to search paths.
# The osxcross toolchain sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE to ONLY,
# which blocks find_package from locating Qt outside the SDK sysroot.
# By appending QT_MAC to CMAKE_FIND_ROOT_PATH *after* the osxcross toolchain
# runs, all Qt6 sub-packages (Widgets, Core, Gui, etc.) become findable.
# ---------------------------------------------------------------------------
TOOLCHAIN_WRAPPER=$(mktemp /tmp/toolchain-qt-XXXXXX.cmake)
cat > "$TOOLCHAIN_WRAPPER" << EOF
include(${OSXCROSS_ROOT}/toolchain.cmake)
list(APPEND CMAKE_FIND_ROOT_PATH "${QT_MAC}")
EOF
trap 'rm -f "$TOOLCHAIN_WRAPPER"' EXIT

# ---------------------------------------------------------------------------
# Common CMake arguments (both CLI and GUI)
# ---------------------------------------------------------------------------
CMAKE_COMMON_ARGS=(
  -DTSMUXER_STATIC_BUILD=ON
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_WRAPPER}"
  -DTSMUXER_GUI=ON
  -DWITHOUT_PKGCONFIG=TRUE
  -DFREETYPE_LIBRARY="${FREETYPE_PREFIX}/lib/libfreetype.a"
  -DFREETYPE_INCLUDE_DIRS="${FREETYPE_PREFIX}/include"
  -DCMAKE_PREFIX_PATH="${QT_MAC}"
  -DQT_HOST_PATH="${QT_HOST}"
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
# Assemble universal app bundle
# ---------------------------------------------------------------------------
rm -rf bin/mac bin/mac.zip
mkdir -p bin/mac
cp -r build-arm64/tsMuxerGUI/tsMuxerGUI.app bin/mac/tsMuxerGUI.app

# Create universal CLI binary
"$LIPO" -create \
  build-arm64/tsMuxer/tsmuxer \
  build-x86_64/tsMuxer/tsmuxer \
  -output bin/mac/tsMuxeR

# Create universal GUI binary
"$LIPO" -create \
  build-arm64/tsMuxerGUI/tsMuxerGUI.app/Contents/MacOS/tsMuxerGUI \
  build-x86_64/tsMuxerGUI/tsMuxerGUI.app/Contents/MacOS/tsMuxerGUI \
  -output bin/mac/tsMuxerGUI.app/Contents/MacOS/tsMuxerGUI

# Embed the universal CLI binary inside the app bundle
cp bin/mac/tsMuxeR bin/mac/tsMuxerGUI.app/Contents/MacOS/tsMuxeR

# ---------------------------------------------------------------------------
# Bundle Qt frameworks into the .app
# ---------------------------------------------------------------------------
FRAMEWORKS_DIR=bin/mac/tsMuxerGUI.app/Contents/Frameworks
mkdir -p "$FRAMEWORKS_DIR"

# Add rpath so the GUI binary can find bundled frameworks
"$INSTALL_NAME_TOOL" -add_rpath @executable_path/../Frameworks \
  bin/mac/tsMuxerGUI.app/Contents/MacOS/tsMuxerGUI

# Copy Qt frameworks referenced by a binary
copy_needed_frameworks() {
  local binary=$1
  for fw_ref in $("$OTOOL" -L "$binary" | awk '/@rpath\/.*\.framework/ { print $1 }'); do
    local fw_name
    fw_name=$(echo "$fw_ref" | sed 's|@rpath/||' | cut -d'/' -f1)
    if [ -d "${QT_MAC}/lib/${fw_name}" ] && [ ! -d "${FRAMEWORKS_DIR}/${fw_name}" ]; then
      echo "Bundling framework: ${fw_name}"
      cp -a "${QT_MAC}/lib/${fw_name}" "${FRAMEWORKS_DIR}/${fw_name}"
    fi
  done
}

# Scan the main binary
copy_needed_frameworks bin/mac/tsMuxerGUI.app/Contents/MacOS/tsMuxerGUI

# ---------------------------------------------------------------------------
# Bundle the cocoa platform plugin
# ---------------------------------------------------------------------------
mkdir -p bin/mac/tsMuxerGUI.app/Contents/plugins/platforms
cp "${QT_MAC}/plugins/platforms/libqcocoa.dylib" \
  bin/mac/tsMuxerGUI.app/Contents/plugins/platforms/

# Add rpath so the cocoa plugin can find bundled frameworks
"$INSTALL_NAME_TOOL" -add_rpath @loader_path/../../Frameworks \
  bin/mac/tsMuxerGUI.app/Contents/plugins/platforms/libqcocoa.dylib

# Scan cocoa plugin for additional framework dependencies
copy_needed_frameworks bin/mac/tsMuxerGUI.app/Contents/plugins/platforms/libqcocoa.dylib

# ---------------------------------------------------------------------------
# Bundle the macOS style plugin (without it Qt falls back to Fusion style)
# ---------------------------------------------------------------------------
mkdir -p bin/mac/tsMuxerGUI.app/Contents/plugins/styles
cp "${QT_MAC}/plugins/styles/libqmacstyle.dylib" \
  bin/mac/tsMuxerGUI.app/Contents/plugins/styles/

# Add rpath so the style plugin can find bundled frameworks
"$INSTALL_NAME_TOOL" -add_rpath @loader_path/../../Frameworks \
  bin/mac/tsMuxerGUI.app/Contents/plugins/styles/libqmacstyle.dylib

# Scan style plugin for additional framework dependencies
copy_needed_frameworks bin/mac/tsMuxerGUI.app/Contents/plugins/styles/libqmacstyle.dylib

# Scan bundled frameworks for transitive dependencies
changed=1
while [ "$changed" -eq 1 ]; do
  changed=0
  for fw_dir in "${FRAMEWORKS_DIR}"/*.framework; do
    [ -d "$fw_dir" ] || continue
    fw_name=$(basename "$fw_dir" .framework)
    fw_binary="${fw_dir}/Versions/A/${fw_name}"
    [ -f "$fw_binary" ] || fw_binary="${fw_dir}/${fw_name}"
    [ -f "$fw_binary" ] || continue
    before=$(ls -d "${FRAMEWORKS_DIR}"/*.framework 2>/dev/null | wc -l)
    copy_needed_frameworks "$fw_binary"
    after=$(ls -d "${FRAMEWORKS_DIR}"/*.framework 2>/dev/null | wc -l)
    if [ "$after" -gt "$before" ]; then
      changed=1
    fi
  done
done

# ---------------------------------------------------------------------------
# Qt configuration so plugins can be found at runtime
# ---------------------------------------------------------------------------
mkdir -p bin/mac/tsMuxerGUI.app/Contents/Resources
cat << 'EOF' > bin/mac/tsMuxerGUI.app/Contents/Resources/qt.conf
[Paths]
Plugins=plugins
EOF

# ---------------------------------------------------------------------------
# Clean up
# ---------------------------------------------------------------------------
rm -rf build-arm64 build-x86_64

ls bin/mac/tsMuxeR
ls bin/mac/tsMuxerGUI.app/Contents/MacOS/tsMuxerGUI
