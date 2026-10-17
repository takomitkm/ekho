#!/bin/bash
# ASCII only. <cstdlib> failed while <cstdio> (same overlay, same headers) built
# fine. Find out which stdlib.h and which sys/cdefs.h are actually being read.
. /c/Users/YiZhidi/x-common.sh
set -u
SRF=$XROOT/kit-sysroot-focal
TC_C=/d/yzd/tools/cross/gcc-arm-10.3-2021.07-mingw-w64-i686-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include

for f in stdlib.h stdio.h string.h features.h sys/cdefs.h bits/types.h; do
    printf '  %-16s overlay: %-10s  toolchain: %-10s\n' "$f" \
        "$([ -f "$SRF/usr/include/$f" ] && echo present || echo MISSING)" \
        "$([ -f "$TC_C/$f" ] && echo present || echo MISSING)"
done

echo
echo "=== __attr_access, the macro the toolchain's stdlib.h wants ==="
for p in "$SRF/usr/include/sys/cdefs.h" "$TC_C/sys/cdefs.h"; do
    printf '  %-70s %s definitions\n' "${p#$XROOT/}" "$(grep -c 'define __attr_access' "$p" 2>/dev/null)"
done
echo "  --- line 935 of each stdlib.h ---"
sed -n '935p' "$SRF/usr/include/stdlib.h" 2>/dev/null | sed 's/^/    overlay:   /'
sed -n '935p' "$TC_C/stdlib.h" 2>/dev/null | sed 's/^/    toolchain: /'

echo
echo "=== which <stdlib.h> does the C++ compiler actually open? ==="
echo '#include <cstdlib>' > /tmp/ct.cpp
"$CXX" -std=gnu++17 -I"$SRF/usr/include" -I"$SRF/usr/include/aarch64-linux-gnu" -E -H /tmp/ct.cpp > /dev/null 2>/tmp/h1.txt
grep -E 'stdlib\.h|sys/cdefs\.h|features\.h' /tmp/h1.txt | sed 's/^/    /' | head -12

echo
echo "=== and with only the overlay, no second -I? ==="
"$CXX" -std=gnu++17 -I"$SRF/usr/include" -E -H /tmp/ct.cpp > /dev/null 2>/tmp/h2.txt
head -3 /tmp/h2.txt | sed 's/^/    /'

echo
echo "=== where does the compiler look for <stdlib.h>, in order? ==="
"$CXX" -E -Wp,-v -x c++ /dev/null 2>&1 | sed -n '/search starts here/,/End of search list/p' | sed 's/^/    /'

echo
echo "=== does the overlay's own stdlib.h compile standalone? ==="
"$CXX" -std=gnu++17 -I"$SRF/usr/include" -I"$SRF/usr/include/aarch64-linux-gnu" -fsyntax-only /tmp/ct.cpp 2>&1 | head -8 | sed 's/^/    /'
