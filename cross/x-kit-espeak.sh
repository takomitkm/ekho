#!/bin/bash
# ASCII only. kit-espeak: libespeak-ng 1.50.
#
# Four things make this one different from libsndfile:
#
#  * The generated phoneme tables are checked into the tarball (espeak-ng-data/,
#    428 files), so nothing has to RUN the aarch64 binary during the build. The
#    usual cross-compile blocker for espeak-ng -- `make all` ends by invoking
#    the espeak-ng it just produced, to compile those tables -- is sidestepped
#    by naming the three targets that can be built and not asking for `all`.
#    Run `make` here and you get, correctly and uselessly:
#        src/espeak-ng: cannot execute binary file: Exec format error
#
#  * The DFSG repack keeps configure/Makefile.in but strips the autotools
#    helpers (install-sh, missing, config.sub, ltmain.sh...), so configure
#    cannot even start. autogen.sh would fix that, but it calls
#    `automake --add-missing` without --copy, and on Windows that means
#    symlinks, which this box will not create. Same steps, --copy instead.
#
#  * espeak-ng builds a program and a library from one Makefile.am and its
#    rules assume an in-tree build (ESPEAK_DATA_PATH=$(PWD) src/espeak-ng ...),
#    so configure runs inside the source directory. Library, headers, .pc and
#    data are collected by hand rather than through `make install`, which would
#    drag the data-compilation rules in behind them.
#
#  * -fcommon: espeak-ng 1.50 declares a few globals without `extern` (e.g.
#    `samplerate` in src/espeak-ng.c and again in wavegen.c). GCC 10 defaults to
#    -fno-common, so linking the two CLI programs fails with "multiple
#    definition". Upstream fixed it later; the library is unaffected either way.
. /c/Users/YiZhidi/x-common.sh
set -u

KIT=$XROOT/kit-espeak
TB="$XROOT/dl/espeak-ng_1.50+dfsg.orig.tar.gz"
DIR=espeak-ng-1.50+dfsg
SRC=$XROOT/src/$DIR
SHIM=$XROOT/kit-glibc-compat/include/ekho-cross-stat.h
LOG=$XROOT/logs/espeak-configure.log

echo "=== kit-espeak: libespeak-ng 1.50 -> $KIT ==="
[ -f "$TB" ] || die "no $TB"
# speech.c calls stat() when it validates the data path. Compiled in, it carries
# an undefined stat into every static link, and the toolchain's glibc 2.33
# resolves that to stat@GLIBC_2.33 -- a version the board's glibc 2.31 has no
# entry for. The shim routes it to __xstat, which every aarch64 glibc exports.
[ -f "$SHIM" ] || die "no glibc shim at $SHIM (run x-kit-glibc-compat.sh first)"
kit_reset "$KIT"

echo "--- fresh source tree (this build runs in-tree) ---"
rm -rf "$SRC"
cd "$XROOT/src" || exit 1
tar -xzf "$TB" >/dev/null 2>&1
[ -d "$SRC" ] || die "extraction failed"
cd "$SRC" || exit 1
# Debian left a generated config.h and a libtool in the tarball; a fresh
# configure overwrites config.h but not the libtool script.
rm -f config.h libtool stamp-h1 Makefile src/Makefile

echo "--- regenerate the autotools files, --copy not --link ---"
# autogen.sh does `ln -sf CHANGELOG.md NEWS` and `ln -sf README.md README`.
# NEWS survived the repack, README did not, and a symlink is not an option on
# Windows -- so satisfy automake's `required file './README'` with a copy.
[ -f NEWS ]   || cp -f CHANGELOG.md NEWS
[ -f README ] || cp -f README.md README
mkdir -p m4
aclocal -I m4             > "$XROOT/logs/espeak-auto.log" 2>&1 || { echo "  aclocal FAILED";    tail -5 "$XROOT/logs/espeak-auto.log" | sed 's/^/    /'; exit 1; }
libtoolize --copy --force >> "$XROOT/logs/espeak-auto.log" 2>&1 || { echo "  libtoolize FAILED"; exit 1; }
autoheader                >> "$XROOT/logs/espeak-auto.log" 2>&1 || { echo "  autoheader FAILED"; exit 1; }
automake --add-missing --copy --force-missing >> "$XROOT/logs/espeak-auto.log" 2>&1 || { echo "  automake FAILED"; tail -5 "$XROOT/logs/espeak-auto.log" | sed 's/^/    /'; exit 1; }
autoconf                  >> "$XROOT/logs/espeak-auto.log" 2>&1 || { echo "  autoconf FAILED"; exit 1; }
echo "  helpers: $(for f in install-sh missing config.sub config.guess ltmain.sh compile depcomp; do printf '%s ' "$(test -e "$f" && echo "$f" || echo "MISSING:$f")"; done)"

echo "--- touch the data so make cannot think it is stale ---"
find espeak-ng-data -type f -exec touch {} + 2>/dev/null
echo "  touched $(find espeak-ng-data -type f | wc -l) files"

echo "--- configure (in-tree) ---"
./configure \
    --host=aarch64-none-linux-gnu \
    --prefix="$KIT" \
    --disable-shared --enable-static \
    --without-pcaudiolib --without-sonic --without-mbrola \
    --without-extdict-ru --without-extdict-zh --without-extdict-zhy \
    CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
    CFLAGS="-O2 -fcommon" \
    CPPFLAGS="-include $SHIM" \
    > "$LOG" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    echo "  configure FAILED rc=$rc"
    tail -25 "$LOG" | sed 's/^/    /'
    exit 1
fi
grep -iE '^ *(Sonic|Klatt|Mbrola|Async|PCAudio|audio)' "$LOG" | head -8 | sed 's/^/    /'

echo "--- make: the three targets that can be built, not 'all' ---"
make -j8 src/libespeak-ng.la src/espeak-ng src/speak-ng > "$XROOT/logs/espeak-make.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    echo "  make FAILED rc=$rc"
    grep -iE 'error|Error' "$XROOT/logs/espeak-make.log" | head -20 | sed 's/^/    /'
    echo "  --- lines where it tried to run a binary it had just built ---"
    grep -nE 'src/espeak-ng .*(--compile|-v )' "$XROOT/logs/espeak-make.log" | head -8 | sed 's/^/    /'
    exit 1
fi
echo "  library: $(ls -1 src/.libs/libespeak-ng.a 2>/dev/null || echo MISSING)"
echo "  programs: $(ls -1 src/espeak-ng src/speak-ng 2>/dev/null | tr '\n' ' ')  (built, not shipped)"

echo "--- collect ---"
[ -f src/.libs/libespeak-ng.a ] || die "no libespeak-ng.a"
mkdir -p "$KIT/lib/pkgconfig" "$KIT/include/espeak-ng" "$KIT/include/espeak" "$KIT/share"
cp src/.libs/libespeak-ng.a "$KIT/lib/libespeak-ng.a"
# espeak-ng installs the same header under both espeak/ and espeak-ng/; mirror
# that so a consumer written against either spelling works.
cp src/include/espeak-ng/speak_lib.h "$KIT/include/espeak-ng/speak_lib.h"
cp src/include/espeak/speak_lib.h    "$KIT/include/espeak/speak_lib.h"
echo "  library: $(du -h "$KIT/lib/libespeak-ng.a" | cut -f1)"
echo "  headers: $(find "$KIT/include" -type f | wc -l)"

# Take the tables straight out of the tarball. `make` above did not touch them,
# but a run of plain `make all` would have left an empty espeak-ng-data/mbrola_ph
# behind, and going back to the tarball is cheaper than reasoning about it.
rm -rf "$KIT/share/espeak-ng-data"
tar -xzf "$TB" -C "$KIT/share" --strip-components=1 "$DIR/espeak-ng-data" 2>/dev/null
echo "  data:    $(find "$KIT/share/espeak-ng-data" -type f | wc -l) files, $(du -sh "$KIT/share/espeak-ng-data" | cut -f1)  (from the tarball, not from make)"

if [ -f espeak-ng.pc ]; then
    sed -e 's|^prefix=.*|prefix=${pcfiledir}/../..|' \
        -e 's|^exec_prefix=.*|exec_prefix=${prefix}|' \
        -e 's|^libdir=.*|libdir=${prefix}/lib|' \
        -e 's|^includedir=.*|includedir=${prefix}/include|' \
        espeak-ng.pc > "$KIT/lib/pkgconfig/espeak-ng.pc"
fi
[ -s "$KIT/lib/pkgconfig/espeak-ng.pc" ] || die "no espeak-ng.pc was produced"

echo "--- verify ---"
echo "  arch: $("$READELF" -h "$KIT/lib/libespeak-ng.a" 2>/dev/null | grep -m1 'Machine:' | sed 's/^ *//')"
echo "  stat references: $(nm -A "$KIT/lib/libespeak-ng.a" 2>/dev/null | grep -cE ' U (stat|fstat|lstat)$') plain (must be 0), $(nm -A "$KIT/lib/libespeak-ng.a" 2>/dev/null | grep -cE ' U __[xf]stat$') through the shim"
echo "  key symbols:"
for s in espeak_Initialize espeak_SetVoiceByName espeak_SetSynthCallback espeak_Synth espeak_SetParameter espeak_Terminate espeak_Cancel espeak_ListVoices; do
    printf '    %-24s %s\n' "$s" "$("$NM" --defined-only "$KIT/lib/libespeak-ng.a" 2>/dev/null | grep -q " T $s\$" && echo present || echo MISSING)"
done
echo "  pkg-config:"
PKG_CONFIG_LIBDIR="$KIT/lib/pkgconfig" pkg-config --modversion espeak-ng 2>&1 | sed 's/^/    /'
PKG_CONFIG_LIBDIR="$KIT/lib/pkgconfig" pkg-config --cflags --libs espeak-ng 2>&1 | sed 's/^/    /'

kit_finish "$KIT" espeak-ng 1.50.0 \
  "espeak-ng_1.50+dfsg.orig.tar.gz, USTC ubuntu-ports pool (focal's version; the generated tables ship in the tarball)" \
  "libespeak-ng.a + speak_lib.h (under espeak/ and espeak-ng/) + espeak-ng.pc + espeak-ng-data/ (428 tables), no audio output"
