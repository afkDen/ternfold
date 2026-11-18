#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
TF_CC=${CC:-cc}
TF_CC=$(command -v "$TF_CC") || { printf '%s\n' 'Install GCC/Clang or set CC to its path.' >&2; exit 2; }
export TF_CC
mkdir -p build
"$TF_CC" -std=c17 -Wall -Wextra -Werror -pedantic -Wconversion -Wshadow tools/tfdev.c -o build/tfdev
exec ./build/tfdev "${1:-check}"
