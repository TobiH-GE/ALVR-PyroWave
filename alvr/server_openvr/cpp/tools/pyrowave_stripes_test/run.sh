#!/bin/bash
# Tests the PyroWave stripe schedule and packet path (PyroWaveStripes.h) against PyroWave's own
# BlockLayout and BitstreamParser. Needs a C++17 compiler and a PyroWave checkout (the commit in
# deps/windows/pyrowave/README.md); no GPU.
#
#   bash run.sh /path/to/pyrowave
set -e
PYROWAVE=${1:?usage: run.sh /path/to/pyrowave}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d)
for test in stripes_test packets_test; do
    c++ -O2 -std=c++17 -I"$HERE/../../platform/win32" -I"$PYROWAVE" \
        "$HERE/$test.cpp" "$PYROWAVE/metal/pyrowave_bitstream.cpp" -o "$OUT/$test"
    "$OUT/$test"
done
rm -rf "$OUT"
