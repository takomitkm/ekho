#!/bin/bash
# ASCII only. ekho_dict.cpp failed to compile, and its message is buried in the
# interleaved -j8 output. Rebuild just that one unit, serially, so the reason is
# readable instead of racing twelve other compilers.
. /c/Users/YiZhidi/x-common.sh
set -u
cd "$XROOT/src/ekho-linux" || exit 1

echo "=== grep the old log for the unit, with context ==="
grep -n 'ekho_dict' "$XROOT/logs/ekho-make.log" | head -20 | sed 's/^/  /'
echo "  --- any of these words? ---"
grep -niE 'fatal|out of memory|virtual memory|killed|terminated|signal|internal compiler' "$XROOT/logs/ekho-make.log" | head -10 | sed 's/^/  /'

echo
echo "=== rebuild that single object, serially ==="
rm -f src/libekho_a-ekho_dict.o
make src/libekho_a-ekho_dict.o 2>&1 | tail -40 | sed 's/^/  /'
echo "  rc=$?"
