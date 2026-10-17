#!/bin/bash
# ASCII only. espeak built fine before the shim and not after, so the shim
# changed something configure decided. HAVE_ENDIAN_H is the macro that gates the
# header that now errors. Check what configure recorded and which objects failed.
. /c/Users/YiZhidi/x-common.sh
set -u
SRC=$XROOT/src/espeak-ng-1.50+dfsg
SHIM=$XROOT/kit-glibc-compat/include/ekho-cross-stat.h

echo "=== doesn't the shim pull in <endian.h> through <sys/types.h>? ==="
for inc in "" "#include <sys/stat.h>" "#include <sys/types.h>"; do
    printf '  %-26s _ENDIAN_H=%s  endian.h-included=%s\n' "${inc:-nothing}" \
        "$( { [ -z "$inc" ] && echo '' || echo "$inc"; } | "$CC" -dM -E -x c - 2>/dev/null | grep -c '^#define _ENDIAN_H')" \
        "$( { [ -z "$inc" ] && echo '' || echo "$inc"; } | "$CC" -E -x c - 2>/dev/null | grep -c 'endian.h')"
done

echo
echo "=== what configure recorded about endianness ==="
grep -nE 'HAVE_(SYS_)?ENDIAN_H|ENDIAN' "$SRC/config.h" 2>/dev/null | sed 's/^/  /'
echo "  --- and in the configure log ---"
grep -nE 'endian' "$XROOT/logs/espeak-configure.log" 2>/dev/null | head -10 | sed 's/^/  /'

echo
echo "=== which objects failed ==="
grep -E 'error' "$XROOT/logs/espeak-make.log" 2>/dev/null | grep -oE '[a-z0-9_-]+\.c|[a-z0-9_-]+\.o' | sort -u | head -12 | sed 's/^/  /'
echo "  --- first error with context ---"
n=$(grep -n 'platform not supported' "$XROOT/logs/espeak-make.log" | head -1 | cut -d: -f1)
sed -n "$((n-12)),$((n+2))p" "$XROOT/logs/espeak-make.log" | sed 's/^/  /'

echo
echo "=== does the endian check pass with the shim? ==="
for flags in "" "-include $SHIM"; do
    cat > /tmp/ec.c <<'EOF'
#include <endian.h>
int main(void) { return 0; }
EOF
    printf '  %-34s ' "check ${flags:-without shim}"
    "$CC" $flags -c /tmp/ec.c -o /tmp/ec.o 2>/tmp/ec.err && echo "PASS" || { echo "FAIL: $(head -2 /tmp/ec.err | tr '\n' ' ')"; }
done

echo
echo "=== and what the header does with HAVE_ENDIAN_H defined ==="
sed -n '19,46p' "$SRC/src/include/compat/endian.h" | cat -n | sed 's/^/  /'
