#!/bin/bash
# ASCII only. Before configuring ekho: can the linker actually resolve -lm,
# -ldl and -lpthread against this sysroot, and does it have the crt objects?
. /c/Users/YiZhidi/x-common.sh
set -u

echo "=== sysroot: the archive/symlink stubs the linker needs ==="
for d in "$TC/aarch64-none-linux-gnu/libc/usr/lib64" "$TC/aarch64-none-linux-gnu/libc/lib64" "$TC/aarch64-none-linux-gnu/lib64"; do
    echo "  --- $d ---"
    ls -1 "$d" 2>/dev/null | grep -E '^(libm|libdl|libpthread|libc|librt|libgcc_s|libstdc\+\+|libatomic|libresolv|crt1|crti|crtn|Scrt1)\.' | sed 's/^/    /'
done

echo
echo "=== sysroot: crt objects and .so stubs, wherever they are ==="
find "$TC/aarch64-none-linux-gnu" -maxdepth 4 \( -name 'crt1.o' -o -name 'Scrt1.o' -o -name 'crti.o' -o -name 'libm.so' -o -name 'libdl.so' -o -name 'libpthread.so' -o -name 'libc.so' -o -name 'libstdc++.so' \) 2>/dev/null | sed 's/^/  /'

echo
echo "=== what those stubs are (script vs symlink) ==="
for f in libm.so libdl.so libpthread.so libc.so; do
    p=$(find "$TC/aarch64-none-linux-gnu" -maxdepth 4 -name "$f" 2>/dev/null | head -1)
    if [ -n "$p" ]; then
        printf '  %-16s %s\n' "$f" "$p"
        head -c 200 "$p" 2>/dev/null | tr -d '\0' | head -3 | sed 's/^/      /'
    else
        printf '  %-16s MISSING\n' "$f"
    fi
done

echo
echo "=== do the three kits resolve through pkg-config together? ==="
export PKG_CONFIG_LIBDIR="$XROOT/kit-sndfile/lib/pkgconfig:$XROOT/kit-espeak/lib/pkgconfig:$XROOT/kit-sonic/lib/pkgconfig"
for m in sndfile espeak-ng sonic; do
    printf '  %-10s %-10s %s\n' "$m" "$(pkg-config --modversion $m 2>&1)" "$(pkg-config --cflags --libs $m 2>&1)"
done

echo
echo "=== a real link through the kits: sndfile + espeak + sonic + pthread + dl + m ==="
D=/tmp/xlink; rm -rf $D; mkdir -p $D
cat > "$D/probe.c" <<'EOF'
#include <sndfile.h>
#include <espeak-ng/speak_lib.h>
#include "sonic.h"
#include <stdio.h>
int main(void) {
    SF_INFO si = {0};
    sonicStream s = sonicCreateStream(22050, 1);
    printf("%s %s\n", sf_version_string(), espeak_VersionInfo ? "espeak" : "?");
    sonicDestroyStream(s);
    (void)si;
    return 0;
}
EOF
"$CC" -O2 -I"$XROOT/kit-sonic/include" "$D/probe.c" \
    "$XROOT/kit-sndfile/lib/libsndfile.a" "$XROOT/kit-espeak/lib/libespeak-ng.a" \
    "$XROOT/kit-sonic/lib/libsonic.a" \
    -lpthread -ldl -lm -o "$D/probe" 2>&1 | head -20 | sed 's/^/    /'
if [ -s "$D/probe" ]; then
    echo "  linked: $(stat -c%s "$D/probe") bytes"
    "$READELF" -d "$D/probe" | grep -o 'Shared library: *\[[^]]*\]' | sed 's/^/    /'
    "$READELF" -V "$D/probe" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -3 | sed 's/^/    /'
else
    echo "  LINK FAILED"
fi
