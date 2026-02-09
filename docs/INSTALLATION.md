# tsMuxer : Installation Instructions

The following executables are created to be portable, so you can just save and extract the compressed package for your platform. 

Nightly builds are created by GitHub Actions and stored as [releases](https://github.com/jaminmc/tsMuxer/releases). Please visit the releases page in order to download the release you're interested in.

## Windows

The ZIP file for Windows can just be unzipped and the executables can be used straight away - there are no dependencies.

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
