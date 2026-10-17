#!/bin/bash
# ASCII only. Which target did the serial build stop on, and what did it say?
. /c/Users/YiZhidi/x-common.sh
set -u
cd "$XROOT/src/ekho-linux" || exit 1

for L in ekho-make.log ekho-make-serial.log; do
    F="$XROOT/logs/$L"
    echo "=== $L ($(stat -c%s "$F" 2>/dev/null) bytes) ==="
    echo "  --- make's own failure lines ---"
    grep -nE 'Error [0-9]|\*\*\*' "$F" 2>/dev/null | head -10 | sed 's/^/    /'
    echo "  --- last 30 lines ---"
    tail -30 "$F" 2>/dev/null | sed 's/^/    /'
    echo
done

echo "=== which objects are missing ==="
for o in src/libekho_a-*.o src/ekho-*.o libekho.a libsonic.a; do
    [ -f "$o" ] || echo "  missing: $o"
done

echo
echo "=== the dependency file that the recipe writes ==="
ls -l src/.deps/ 2>/dev/null | head -8 | sed 's/^/  /'

echo
echo "=== does the exact recipe work when run by hand, now that the box is calm? ==="
rm -f src/libekho_a-ekho_dict.o
CMD=$(make V=1 -n src/libekho_a-ekho_dict.o 2>/dev/null | grep -m1 'g++')
eval "$CMD"
echo "  exact recipe rc=$?  obj=$([ -f src/libekho_a-ekho_dict.o ] && stat -c%s src/libekho_a-ekho_dict.o || echo absent)"

echo
echo "=== and via make itself ==="
rm -f src/libekho_a-ekho_dict.o
make V=1 src/libekho_a-ekho_dict.o 2>&1 | tail -5 | sed 's/^/  /'
echo "  make rc=$?  obj=$([ -f src/libekho_a-ekho_dict.o ] && stat -c%s src/libekho_a-ekho_dict.o || echo absent)"
