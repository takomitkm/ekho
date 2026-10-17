#!/bin/bash
# ASCII only. The shim covered the tree's own translation units but GLIBC_2.33 is
# still needed. ekho links libsndfile.a and libespeak-ng.a statically, and those
# were built before the shim existed. Confirm that is where it comes from.
. /c/Users/YiZhidi/x-common.sh
set -u
S=$XROOT/src/ekho-linux

echo "=== undefined stat/fstat references inside the static libraries ==="
for a in "$XROOT/kit-sndfile/lib/libsndfile.a" "$XROOT/kit-espeak/lib/libespeak-ng.a" "$XROOT/kit-sonic/lib/sonic.a"; do
    [ -f "$a" ] || continue
    n=$(nm -A "$a" 2>/dev/null | grep -cE ' U (stat|fstat|lstat|fstatat)$')
    printf '  %-46s %s undefined stat-family references\n' "$(basename "$a")" "$n"
    nm -A "$a" 2>/dev/null | grep -E ' U (stat|fstat|lstat|fstatat)$' | head -4 | sed 's/^/      /'
done

echo
echo "=== what the linked binary asks for now ==="
for b in ekho ekho-stream ekho-server; do
    printf '  %-12s ' "$b"
    "$READELF" -sW --dyn-syms "$S/$b" 2>/dev/null | grep -oE '[a-z_]+@GLIBC_2\.[0-9]+' \
        | grep -i stat | sort -u | tr '\n' ' '
    echo
    printf '               oversize needs: '
    "$READELF" -V "$S/$b" 2>/dev/null | grep -oE 'GLIBC_2\.(3[0-9]|[4-9][0-9])' | sort -u | tr '\n' ' '
    echo
done

echo
echo "=== and what the toolchain's libc actually defines ==="
"$NM" -D --defined-only "$TC/aarch64-none-linux-gnu/libc/lib64/libc.so.6" 2>/dev/null \
    | grep -E ' (stat|fstat|lstat)$' | sed 's/^/  /'
echo "  --- the versioned form ---"
"$READELF" -sW --dyn-syms "$TC/aarch64-none-linux-gnu/libc/lib64/libc.so.6" 2>/dev/null \
    | grep -E ' (stat|fstat|lstat)@' | sed 's/^/  /'

echo
echo "=== which object files in the ekho build still reference stat ==="
for o in $(find "$S" -name '*.o' 2>/dev/null); do
    if nm "$o" 2>/dev/null | grep -qE ' U (stat|fstat)$'; then
        printf '  %s\n' "${o#$S/}"
    fi
done | head -20
