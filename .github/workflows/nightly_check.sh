#!/usr/bin/env bash

# Nightly Build Check Script
# 
# This script determines whether a new nightly build is needed by checking
# if the current HEAD commit already has a nightly tag.
#
# Exit codes:
#   0 = Build is needed (HEAD has no nightly tag)
#   1 = Build is not needed (HEAD already has nightly tag)

set -e

# Check if the current HEAD contains a nightly tag
# If git describe succeeds, HEAD is tagged as a nightly build
if git describe --tags --contains --match 'nightly-*' HEAD >/dev/null 2>&1; then
  # HEAD already has a nightly tag - no new build needed
  exit 1
else
  # HEAD does not have a nightly tag - new build is needed
  echo "$(date +%Y-%m-%d-%H-%M-%S)"
  exit 0
fi
