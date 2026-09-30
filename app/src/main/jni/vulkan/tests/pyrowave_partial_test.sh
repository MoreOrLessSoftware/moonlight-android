#!/bin/sh
# Builds and runs pyrowave_partial_test.cpp on the host. It needs a Vulkan GPU and a desktop
# build of PyroWave:
#   PYROWAVE_DIR   PyroWave checkout (default: a pyrowave folder next to the moonlight-android folder)
#   PYROWAVE_LIB   folder with its import library (default: $PYROWAVE_DIR/output/lib)
#   PYROWAVE_BIN   folder with its shared library (default: $PYROWAVE_DIR/output/bin)
#   CXX            compiler (default: g++)
set -e
cd "$(dirname "$0")"
REPO=$(cd ../../../../../.. && pwd)
PYROWAVE_DIR=${PYROWAVE_DIR:-$REPO/../../pyrowave}
PYROWAVE_LIB=${PYROWAVE_LIB:-$PYROWAVE_DIR/output/lib}
PYROWAVE_BIN=${PYROWAVE_BIN:-$PYROWAVE_DIR/output/bin}
OUT=${TMPDIR:-/tmp}/pyrowave_partial_test

mkdir -p "$OUT"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -I.. -I"$PYROWAVE_DIR" \
    -I"$PYROWAVE_DIR/Granite/third_party/khronos/vulkan-headers/include" \
    pyrowave_partial_test.cpp -L"$PYROWAVE_LIB" -lpyrowave-shared -o "$OUT/pyrowave_partial_test"
PATH="$PYROWAVE_BIN:$PATH" LD_LIBRARY_PATH="$PYROWAVE_LIB:$LD_LIBRARY_PATH" "$OUT/pyrowave_partial_test"
