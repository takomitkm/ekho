#!/bin/bash
# ASCII only. kit-glibc-compat: one header, and the reason it is needed.
#
# The Arm 10.3 toolchain ships a sysroot built on glibc 2.33. The board runs
# 2.31. That mismatch is invisible until a symbol's *version* moved between the
# two, and in this tree exactly three did: stat, fstat and lstat, which glibc
# only began exporting as real symbols in 2.33. Before that, and on every
# aarch64 glibc from 2.17 onwards, the headers route them through __xstat,
# __fxstat and __lxstat. So a build against the 2.33 headers emits
# stat@GLIBC_2.33, a 2.31 libc has no version entry by that name, and the
# program does not start.
#
# The first attempt at this was the honest one: pin the headers to focal's
# glibc 2.31 (kit-sysroot-focal). It does not work, and the reason is worth
# recording -- libstdc++'s <cstdlib> reaches the C headers with
# `#include_next <stdlib.h>`, which by definition skips anything added ahead of
# the C++ directory. So a -I overlay pins <sys/stat.h> when it is included
# directly, and does nothing when the same header arrives through <cstdlib>.
# The result is 2.31 headers mixed into 2.33 headers, and the failure is
# `__attr_access` -- a macro 2.33's stdlib.h wants and 2.31's cdefs.h has never
# heard of. That kit is left in place as the record of the attempt; this is what
# the build actually uses.
#
# This is a build-side shim and not a patch to the tree on purpose: it is a
# property of this toolchain and this target, not of ekho. The general fix is a
# sysroot built for the target's glibc; this is the narrow version of it, and it
# works at any -O level, which the header overlay did not.
. /c/Users/YiZhidi/x-common.sh
set -u

KIT=$XROOT/kit-glibc-compat
SRF=$XROOT/kit-sysroot-focal

echo "=== kit-glibc-compat -> $KIT ==="
kit_reset "$KIT"
mkdir -p "$KIT/include"

cat > "$KIT/include/ekho-cross-stat.h" <<'EOF'
/* ekho-cross-stat.h -- forced include for the aarch64 cross build.
 *
 * The Arm GNU Toolchain 10.3 sysroot is glibc 2.33; the board is glibc 2.31.
 * glibc 2.33 is where stat(), fstat() and lstat() stopped being header macros
 * over __xstat() and became exported symbols in their own right. Compiling
 * against 2.33's headers therefore emits a call to stat@GLIBC_2.33, and a 2.31
 * libc has no version entry by that name -- the loader refuses the program.
 *
 * Every aarch64 glibc since the port began (2.17 upwards) exports
 * __xstat/__fxstat/__lxstat at GLIBC_2.17, and the board's does too. Point the
 * calls at those. aarch64 has only ever had one struct stat layout, so the
 * leading version argument is unused and 0 is correct.
 *
 * Only active for aarch64 Linux, so it cannot change a native build.
 */
#ifndef EKHO_CROSS_STAT_H
#define EKHO_CROSS_STAT_H

#include <sys/stat.h>

/* <sys/stat.h> and nothing else. Do not add <sys/types.h>: glibc's
 * <sys/types.h> includes <endian.h> under __USE_MISC, and a source tree that
 * ships its own endian.h on the include path gets that one instead -- espeak-ng
 * does (src/include/compat/endian.h). Because this header is force-included, it
 * then arrives at the top of every translation unit, before the unit has read
 * config.h, so HAVE_ENDIAN_H is not defined yet and espeak-ng's header fails
 * with "#error platform not supported". <sys/stat.h> does not pull in
 * <endian.h>, and it is all that is needed: struct stat must be complete for
 * the declarations below. */

#if defined(__aarch64__) && defined(__linux__)

#ifdef __cplusplus
extern "C" {
#endif

int __xstat(int, const char *, struct stat *);
int __fxstat(int, int, struct stat *);
int __lxstat(int, const char *, struct stat *);
int __fxstatat(int, int, const char *, struct stat *, int);

#ifdef __cplusplus
}
#endif

#define stat(path, buf)       __xstat(0, (path), (buf))
#define fstat(fd, buf)        __fxstat(0, (fd), (buf))
#define lstat(path, buf)      __lxstat(0, (path), (buf))
#define fstatat(fd, p, b, f)  __fxstatat(0, (fd), (p), (b), (f))

#endif /* aarch64 && linux */
#endif /* EKHO_CROSS_STAT_H */
EOF
echo "  include/ekho-cross-stat.h ($(stat -c%s "$KIT/include/ekho-cross-stat.h") bytes)"

SH=$KIT/include/ekho-cross-stat.h
P=/tmp/shimverify; rm -rf $P; mkdir -p $P

# Deliberately a C++ file that goes through <cstdlib> and <filesystem>, because
# that is the path the header overlay could not pin.
cat > $P/p.cpp <<'EOF'
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
int main(int argc, char **argv) {
    struct stat st;
    bool e = std::filesystem::exists(argv[0]);
    int fd = open(argv[0], O_RDONLY);
    if (fd >= 0) fstat(fd, &st);
    stat(argv[0], &st);
    std::thread t([]{});
    t.join();
    std::printf("%d %d\n", (int)e, (int)st.st_size);
    return argc > 99;
}
EOF

report() {  # report <label> <flags...>
    local label=$1; shift
    local out=$P/o; rm -f $out
    "$CXX" -std=gnu++17 "$@" $P/p.cpp -o $out -lpthread -ldl -lm 2>$P/err
    if [ ! -f $out ]; then
        printf '  %-30s BUILD FAILED: %s\n' "$label" "$(head -1 $P/err | cut -c1-90)"
        head -4 $P/err | sed 's/^/        /'
        return 1
    fi
    local max syms
    max=$("$READELF" -V $out 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
    syms=$("$READELF" -sW --dyn-syms $out 2>/dev/null | grep -oE '[a-z_]+@GLIBC_2\.[0-9]+' \
           | grep -E 'stat' | sort -u | tr '\n' ' ')
    printf '  %-30s max=%-13s %s\n' "$label" "$max" "$syms"
    return 0
}

echo
echo "=== C++ (<cstdlib> + <filesystem> + <thread>) through the real headers ==="
rc=0
report "no shim, -O2 (control)" -O2                        || rc=1
report "shim, -O2"            -O2 -include "$SH"           || rc=1
report "shim, -O0"            -O0 -include "$SH"           || rc=1
report "shim, -O2, no -pthread" -O2 -include "$SH"         || rc=1

echo
echo "=== and the same file through the focal header overlay, to record why it failed ==="
if [ -f "$SRF/usr/include/sys/stat.h" ]; then
    report "overlay, -O2" -O2 -I"$SRF/usr/include" -I"$SRF/usr/include/aarch64-linux-gnu" \
        && echo "      (it built -- see the wiki note)" \
        || echo "      ^ 2.33 stdlib.h with 2.31 cdefs.h: this is why the overlay was dropped"
else
    echo "  (kit-sysroot-focal is not on this machine)"
fi

echo
echo "=== regression guard: a tree that ships its own endian.h (espeak-ng does) ==="
# This is the failure that was actually hit. A tree with src/include/compat/
# endian.h on the include path resolves glibc's <sys/types.h> -> <endian.h> to
# its own header, which needs HAVE_ENDIAN_H from config.h. If the shim pulls
# <sys/types.h> in first, config.h has not been read yet and the build dies with
# "#error platform not supported". Reproduce exactly that shape.
mkdir -p "$P/selfinc"
cat > "$P/selfinc/endian.h" <<'EOF'
#ifndef ENDIAN_H_COMPAT_SHIM
#define ENDIAN_H_COMPAT_SHIM
#if defined(HAVE_ENDIAN_H)
/* the real one would be include_next'd here */
#else
# error platform not supported
#endif
#endif
EOF
cat > "$P/selfinc/config.h" <<'EOF'
#define HAVE_ENDIAN_H 1
EOF
cat > "$P/selfinc/tu.c" <<'EOF'
#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
int main(void) { struct stat s; (void)s; return 0; }
EOF
if "$CC" -I"$P/selfinc" -include "$SH" -c "$P/selfinc/tu.c" -o "$P/selfinc/tu.o" 2>"$P/selfinc/err"; then
    echo "  PASS  shim + self-included endian.h + config.h"
else
    echo "  FAIL  $(head -2 "$P/selfinc/err" | tr '\n' ' ' | cut -c1-100)"
    rc=1
fi

echo
echo "  board glibc is $BOARD_GLIBC_MAX -- anything above that must not appear"

kit_finish "$KIT" glibc-compat 2.31 \
  "forced-include header for the cross build: routes stat/fstat/lstat to the symbols every aarch64 glibc has" \
  "include/ekho-cross-stat.h (pass it with -include; inert unless __aarch64__ && __linux__)"

echo
[ $rc -eq 0 ] && echo "== redirect verified in C++ at -O0 and -O2" || echo "== VERIFICATION FAILED"
exit $rc
