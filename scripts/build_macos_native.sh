#!/usr/bin/env bash

set -ex

export MACOSX_DEPLOYMENT_TARGET=10.15

if [ "${CMAKE_OSX_ARCHITECTURES:-}" = "x86_64" ]; then
  # Install a separate x86_64 Homebrew at /usr/local for Intel libraries
  NONINTERACTIVE=1 arch -x86_64 /bin/bash -c \
    "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
  X86_BREW=/usr/local/bin/brew
  arch -x86_64 $X86_BREW install freetype
  BREW_PREFIX=$($X86_BREW --prefix)
else
  brew install freetype
  BREW_PREFIX=$(brew --prefix)
fi

mkdir build

pushd build
CMAKE_ARCH_FLAG=""
if [ -n "${CMAKE_OSX_ARCHITECTURES:-}" ]; then
  CMAKE_ARCH_FLAG="-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
fi

cmake -DCMAKE_BUILD_TYPE=Release -DTSMUXER_STATIC_BUILD=TRUE \
  "-DFREETYPE_LDFLAGS=bz2;${BREW_PREFIX}/lib/libpng.a" -DTSMUXER_GUI=TRUE \
  -DWITHOUT_PKGCONFIG=TRUE ${CMAKE_ARCH_FLAG} \
  -DCMAKE_PREFIX_PATH="${BREW_PREFIX}" \
  -DFREETYPE_LIBRARY="${BREW_PREFIX}/lib/libfreetype.a" \
  -DFREETYPE_INCLUDE_DIR_freetype2="${BREW_PREFIX}/include/freetype2" \
  -DFREETYPE_INCLUDE_DIR_ft2build="${BREW_PREFIX}/include/freetype2" ..

if ! num_cores=$(sysctl -n hw.logicalcpu); then
  num_cores=1
fi

make -j${num_cores}

pushd tsMuxerGUI
pushd tsMuxerGUI.app/Contents
# avoid permission denied errors with Info.plist
chmod 664 "$PWD/Info.plist"
defaults write "$PWD/Info.plist" NSPrincipalClass -string NSApplication
defaults write "$PWD/Info.plist" NSHighResolutionCapable -string True
plutil -convert xml1 Info.plist
popd
macdeployqt tsMuxerGUI.app
popd

mkdir bin
pushd bin
mv ../tsMuxer/tsmuxer tsMuxeR
mv ../tsMuxerGUI/tsMuxerGUI.app .
cp tsMuxeR tsMuxerGUI.app/Contents/MacOS/
zip -9 -r mac.zip tsMuxeR tsMuxerGUI.app
popd
popd
