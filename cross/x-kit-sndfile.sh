#!/bin/bash
# ASCII only. kit-sndfile: libsndfile 1.0.28.
#
# Version 1.0.28 is not a guess. The board's factory rootfs carries
# libsndfile.so.1.0.28, so 1.0.28 is the version this fork was validated against
# and the one a dynamic link would close against. It is linked statically here
# (see below), so there is no libsndfile DT_NEEDED to answer -- but the version
# match is what makes that a choice rather than a risk.
#
# External codecs are off on purpose. Ekho reads and writes WAV and the
# occasional GSM voice blob; it never opens a FLAC, Ogg or Vorbis file, and
# dragging those in would mean cross-building four more libraries to prove
# something the program cannot exercise.
. /c/Users/YiZhidi/x-common.sh
set -u

KIT=$XROOT/kit-sndfile
SRC=$XROOT/src/libsndfile-1.0.28
BD=$XROOT/build/sndfile
SHIM=$XROOT/kit-glibc-compat/include/ekho-cross-stat.h
LOG=$XROOT/logs/sndfile-configure.log

echo "=== kit-sndfile: libsndfile 1.0.28 -> $KIT ==="
[ -f "$SRC/configure" ] || die "no $SRC/configure"
# libsndfile calls stat() in macos.o and fstat() in file_io.o. Those are carried
# into whatever links this library statically, so without the shim ekho ends up
# importing stat@GLIBC_2.33 -- a version the board's glibc 2.31 has no entry for,
# even though nothing in ekho's own sources calls stat any more.
[ -f "$SHIM" ] || die "no glibc shim at $SHIM (run x-kit-glibc-compat.sh first)"
kit_reset "$KIT"
rm -rf "$BD"; mkdir -p "$BD"
cd "$BD" || die "no $BD"

echo "--- configure ---"
"$SRC/configure" \
    --host=aarch64-none-linux-gnu \
    --prefix="$KIT" \
    --enable-static --disable-shared \
    --disable-external-libs --disable-sqlite --disable-alsa \
    --disable-full-suite --disable-octave \
    --disable-dependency-tracking \
    CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
    CPPFLAGS="-include $SHIM" \
    > "$LOG" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    echo "  configure FAILED rc=$rc"
    tail -30 "$LOG" | sed 's/^/    /'
    exit 1
fi
grep -E '^checking (for|whether).*(gsm|GSM|sndfile|largefile|sizeof)' "$LOG" | tail -8 | sed 's/^/    /'
echo "  --- summary ---"
grep -E 'External libs|GSM 6|Compiling with|Host system' "$LOG" | sed 's/^/    /'

echo "--- make ---"
make -j8 > "$XROOT/logs/sndfile-make.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    echo "  make FAILED rc=$rc"
    grep -iE 'error|Error' "$XROOT/logs/sndfile-make.log" | head -25 | sed 's/^/    /'
    exit 1
fi
echo "  ok"

echo "--- install ---"
make install > "$XROOT/logs/sndfile-install.log" 2>&1 || { echo "  install FAILED"; tail -20 "$XROOT/logs/sndfile-install.log" | sed 's/^/    /'; exit 1; }
find "$KIT" -type f | sed 's/^/  /'

# libsndfile writes an absolute prefix into sndfile.pc, which would pin the kit
# to this machine. Rewrite it against ${pcfiledir} so the kit is relocatable.
PC=$KIT/lib/pkgconfig/sndfile.pc
if [ -f "$PC" ]; then
    sed -i 's|^prefix=.*|prefix=${pcfiledir}/../..|' "$PC"
fi

echo "--- verify ---"
[ -f "$KIT/lib/libsndfile.a" ] || die "no libsndfile.a"
[ -f "$KIT/include/sndfile.h" ] || die "no sndfile.h"
echo "  arch: $(find "$KIT/lib/libsndfile.a" -type f -name '*.o' >/dev/null; "$READELF" -h "$KIT/lib/libsndfile.a" 2>/dev/null | grep -m1 'Machine:' | sed 's/^ *//')"
echo "  GMEMBERS  $(find "$KIT/lib" -name '*.a' -exec ls -l {} \; | awk '{print $5" "$9}')"
echo "  stat-family references: $(nm -A "$KIT/lib/libsndfile.a" 2>/dev/null | grep -cE ' U (stat|fstat|lstat)$') plain (must be 0), $(nm -A "$KIT/lib/libsndfile.a" 2>/dev/null | grep -cE ' U __[xf]stat$') through the shim"
echo "  .pc:"
sed 's/^/    /' "$PC"
echo "  version reported by pkg-config:"
PKG_CONFIG_LIBDIR="$KIT/lib/pkgconfig" pkg-config --modversion sndfile 2>&1 | sed 's/^/    /'
PKG_CONFIG_LIBDIR="$KIT/lib/pkgconfig" pkg-config --cflags --libs sndfile 2>&1 | sed 's/^/    /'

kit_finish "$KIT" sndfile 1.0.28 \
  "libsndfile_1.0.28.orig.tar.gz, USTC ubuntu-ports pool (matches the board's libsndfile.so.1.0.28)" \
  "libsndfile.a + sndfile.h + sndfile.pc, built with external codecs disabled (WAV/GSM610 only)"
