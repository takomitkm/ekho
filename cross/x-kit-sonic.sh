#!/bin/bash
# ASCII only. kit-sonic: libsonic, the speed/pitch resampler.
#
# libsonic is vendored inside the ekho tree (third_party/sonic, two files, no
# dependencies) and the ekho build compiles it from there -- see Makefile.am's
# libsonic_a_SOURCES. The kit exists anyway, because "three libraries, kept
# apart, easy to move" is only true if the one that happens to be a text file
# is also a thing you can hand to another project.
. /c/Users/YiZhidi/x-common.sh
set -u

KIT=$XROOT/kit-sonic
SRC=$XROOT/src/ekho-linux/third_party/sonic
BD=$XROOT/build/sonic

echo "=== kit-sonic: libsonic (vendored) -> $KIT ==="
[ -f "$SRC/sonic.c" ] || die "no $SRC/sonic.c"
kit_reset "$KIT"
rm -rf "$BD"; mkdir -p "$BD" "$KIT/include" "$KIT/lib"
cd "$BD" || die "no $BD"

echo "--- compile ---"
"$CC" -O2 -fPIC -I"$SRC" -c "$SRC/sonic.c" -o sonic.o 2>&1 | head -20
[ -f sonic.o ] || die "sonic.c did not compile"
"$AR" rcs "$KIT/lib/libsonic.a" sonic.o
"$RANLIB" "$KIT/lib/libsonic.a"
cp "$SRC/sonic.h" "$KIT/include/"

# libsonic ships no pkg-config file upstream, so write one. ${pcfiledir} keeps
# it valid wherever the kit is moved to.
mkdir -p "$KIT/lib/pkgconfig"
cat > "$KIT/lib/pkgconfig/sonic.pc" <<'EOF'
prefix=${pcfiledir}/../..
exec_prefix=${prefix}
libdir=${prefix}/lib
includedir=${prefix}/include

Name: sonic
Description: Simple library to speed up or slow down speech
Version: 0.2.2-vendored
Libs: -L${libdir} -lsonic
Cflags: -I${includedir}
EOF

echo "--- verify ---"
"$AR" t "$KIT/lib/libsonic.a" | sed 's/^/  member: /'
echo "  arch:   $("$READELF" -h "$KIT/lib/libsonic.a" 2>/dev/null | grep -m1 'Machine:' | sed 's/^ *//')"
echo "  symbols: $("$NM" --defined-only "$KIT/lib/libsonic.a" 2>/dev/null | grep -c ' T ') text symbols"
echo "  exports needed by ekho: $(for s in sonicCreateStream sonicWriteShortToStream sonicReadShortFromStream sonicFlushStream sonicSetSpeed sonicSetPitch sonicSetRate sonicDestroyStream; do "$NM" --defined-only "$KIT/lib/libsonic.a" 2>/dev/null | grep -q " T $s\$" && printf '%s ' "$s"; done)"

kit_finish "$KIT" sonic 0.2.2-vendored \
  "vendored copy, third_party/sonic in the ekho-linux fork" \
  "libsonic.a + sonic.h + sonic.pc"
