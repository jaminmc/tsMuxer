# tsMuxer : Installation Instructions

The following executables are created to be portable, so you can just save and extract the compressed package for your platform. 

Nightly builds are created by GitHub Actions and stored as [releases](https://github.com/jaminmc/tsMuxer/releases). Please visit the releases page in order to download the release you're interested in.

## Windows

The ZIP file for Windows can just be unzipped and the executables can be used straight away - there are no dependencies.

**Windows 7 Users:** Two builds are available for your architecture:
- **64-bit:** `tsMuxer-w64-qt5*.zip` - For 64-bit Windows 7
- **32-bit:** `tsMuxer-w32-qt5*.zip` - For 32-bit Windows 7

Also available (requires Windows 8+):
- `tsMuxer-w64*.zip` - 64-bit with Qt6 GUI
- `tsMuxer-w32*.zip` - 32-bit with Qt6 GUI

Download the appropriate Qt5 build for your Windows 7 system architecture.

## Linux

The ZIP file for Linux can just be unzipped and the executables can be used straight away - there are no dependencies.

## MacOS

The ZIP file for MacOS can just be unzipped, as the executables should be relocatable. 

If you receive missing dependency errors you may need to install a couple of dependencies, using the commands below in the Terminal:
```
ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)" < /dev/null 2> /dev/null
brew install freetype
brew install zlib
```
