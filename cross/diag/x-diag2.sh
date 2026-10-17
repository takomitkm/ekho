#!/bin/bash
# ASCII only. Two things to settle about the espeak kit: what the full make
# tripped over, and where espeak-ng really installs speak_lib.h.
. /c/Users/YiZhidi/x-common.sh
set -u
S=$XROOT/src/espeak-ng-1.50+dfsg
cd "$S" || exit 1

echo "=== what did the full make fail on? ==="
grep -nE 'Error|error:|make\[[0-9]+\]: \*\*\*' "$XROOT/logs/espeak-make.log" | head -12 | sed 's/^/  /'
echo "  --- the lines around the last failure ---"
tail -25 "$XROOT/logs/espeak-make.log" | sed 's/^/  /'

echo
echo "=== where does espeak-ng install its headers? ==="
grep -nE 'include_HEADERS|nobase_include_HEADERS|espeak-ng/speak_lib.h' Makefile.am Makefile 2>/dev/null | sort -u | head -20 | sed 's/^/  /'

echo
echo "=== every speak_lib.h in the tree, with sizes ==="
find . -name 'speak_lib.h' -printf '  %s  %p\n' 2>/dev/null

echo
echo "=== and what is already in the kit ==="
find "$XROOT/kit-espeak/include" -type f -printf '  %s  %p\n' | sed "s|$XROOT/kit-espeak/||"

echo
echo "=== does the kit header match the source one? ==="
md5sum $(find . -name speak_lib.h | head -1) "$XROOT/kit-espeak/include/espeak-ng/speak_lib.h" 2>/dev/null | sed 's/^/  /'
