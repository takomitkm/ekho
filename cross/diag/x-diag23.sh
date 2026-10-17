#!/bin/bash
# ASCII only. Forcing in the shim broke espeak's own compat/endian.h. Look at what
# that header needs, and what the shim's includes change underneath it.
. /c/Users/YiZhidi/x-common.sh
set -u
SRC=$XROOT/src/espeak-ng-1.50+dfsg
SHIM=$XROOT/kit-glibc-compat/include/ekho-cross-stat.h
F=$SRC/src/include/compat/endian.h

echo "=== the header, from line 80 ==="
sed -n '80,112p' "$F" | cat -n | sed 's/^/  /'

echo
echo "=== what it is guarded by, and what it includes ==="
grep -nE '#(if|elif|else|endif|define|include|error)' "$F" | head -30 | sed 's/^/  /'

echo
echo "=== the macros it cares about, before and after the shim ==="
for inc in "" "#include <sys/stat.h>" "#include <sys/types.h>" "#include <stdio.h>"; do
    printf '  %-28s ' "${inc:-nothing}"
    if [ -z "$inc" ]; then
        printf '%s\n' "$(echo '' | "$CC" -dM -E -x c - 2>/dev/null | grep -cE '__BYTE_ORDER|_ENDIAN_H')"
    else
        printf '%s\n' "$(echo "$inc" | "$CC" -dM -E -x c - 2>/dev/null | grep -cE '__BYTE_ORDER|_ENDIAN_H')"
    fi
done
echo "  --- specifically ---"
for inc in "#include <sys/stat.h>" "#include <sys/types.h>"; do
    echo "    $inc"
    echo "$inc" | "$CC" -dM -E -x c - 2>/dev/null | grep -E '_ENDIAN_H|__BYTE_ORDER|__LITTLE_ENDIAN|_SYS_STAT_H' | sed 's/^/      /'
done

echo
echo "=== does the shim's include of sys/stat.h pull in endian.h transitively? ==="
echo '#include <sys/stat.h>' | "$CC" -E -H -x c - >/dev/null 2>/tmp/e1.txt
grep -cE 'endian\.h' /tmp/e1.txt | sed 's/^/  endian.h appearances from sys\/stat.h: /'
echo '#include <sys/types.h>' | "$CC" -E -H -x c - >/dev/null 2>/tmp/e2.txt
grep -cE 'endian\.h' /tmp/e2.txt | sed 's/^/  endian.h appearances from sys\/types.h: /'

echo
echo "=== reproduce the espeak failure, with and without the shim ==="
cat > /tmp/ee.c <<'EOF'
#include "compat/endian.h"
int main(void) { return 0; }
EOF
echo "  --- without the shim ---"
"$CC" -I"$SRC/src/include" -fsyntax-only /tmp/ee.c 2>&1 | head -4 | sed 's/^/      /'
echo "      rc=$?"
echo "  --- with the shim ---"
"$CC" -I"$SRC/src/include" -include "$SHIM" -fsyntax-only /tmp/ee.c 2>&1 | head -6 | sed 's/^/      /'
echo "      rc=$?"

echo
echo "=== how many espeak objects reference stat ==="
nm -A "$XROOT/kit-espeak/lib/libespeak-ng.a" 2>/dev/null | grep -E ' U (stat|fstat)$' | sed 's/^/  /'
echo "  --- and where in the source ---"
grep -rn '\bstat *(' "$SRC/src/libespeak-ng/speech.c" 2>/dev/null | head -5 | sed 's/^/  /'
