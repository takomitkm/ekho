#!/bin/bash
# ASCII only. The C++17 probe passed before the overlay and fails after it, so
# the overlay's glibc 2.31 headers conflict with something. Read the actual
# message instead of guessing.
. /c/Users/YiZhidi/x-common.sh
set -u
cd "$XROOT/src/ekho-linux" || exit 1
L=config.log

echo "=== the C++17 probe, as recorded ==="
n=$(grep -n 'can compile the C++17' "$L" | head -1 | cut -d: -f1)
[ -n "$n" ] || { echo "  probe not in config.log"; exit 1; }
sed -n "$((n-2)),$((n+40))p" "$L" | sed 's/^/  /'
