#!/usr/bin/env bash
#
# Shared helpers for osxcross build scripts.
# Source this file; do not execute directly.
#

readonly OSXCROSS_ROOT=/usr/lib/osxcross
readonly FREETYPE_PREFIX=${OSXCROSS_ROOT}/freetype

# MACOSX_DEPLOYMENT_TARGET is set by the osxcross toolchain.cmake.
# Export it here as well so that non-CMake tools (e.g. plain cc invocations)
# pick up the same minimum version.  Adjust if your SDK targets differ.
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-10.15}

# ---------------------------------------------------------------------------
# Detect osxcross tool names for both architectures.
#
# Usage: find_tool <arch> <tool>
#   arch: arm64 | aarch64 | x86_64
#   tool: cmake | lipo | otool | install_name_tool | ...
# ---------------------------------------------------------------------------
find_tool() {
  local arch=$1 tool=$2
  local f
  for f in "${OSXCROSS_ROOT}/bin/${arch}-apple-darwin"*"-${tool}"; do
    [ -x "$f" ] && echo "$f" && return 0
  done
  # arm64 / aarch64 are interchangeable in osxcross
  if [ "$arch" = "arm64" ]; then
    for f in "${OSXCROSS_ROOT}/bin/aarch64-apple-darwin"*"-${tool}"; do
      [ -x "$f" ] && echo "$f" && return 0
    done
  elif [ "$arch" = "aarch64" ]; then
    for f in "${OSXCROSS_ROOT}/bin/arm64-apple-darwin"*"-${tool}"; do
      [ -x "$f" ] && echo "$f" && return 0
    done
  fi
  echo "ERROR: osxcross tool not found: ${arch}-${tool}" >&2
  return 1
}
