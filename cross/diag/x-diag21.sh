#!/bin/bash
# ASCII only. Two things at once.
#
# 1. Why the overlay loses for <cstdlib>: check whether libstdc++ reaches the C
#    headers through #include_next, which would skip anything added with -I ahead
#    of the C++ directory.
#
# 2. A targeted alternative that does not mix glibc versions at all: keep the
#    toolchain's headers, and redirect just the two symbols whose version moved
#    between glibc 2.33 and 2.31. stat() and fstat() became real exported symbols
#    in 2.33; before that every aarch64 glibc routes them through __xstat and
#    __fxstat, exported at GLIBC_2.17 and present on the board. A forced-include
#    header makes those calls ask for the older symbol, at any -O level.
. /c/Users/YiZhidi/x-common.sh
set -u
SRF=$XROOT/kit-sysroot-focal
GXXINC=$TC/aarch64-none-linux-gnu/include/c++/10.3.1
INC=$XROOT/cross-include

echo "=== does libstdc++ use #include_next to reach the C headers? ==="
grep -n 'include_next' "$GXXINC/cstdlib" "$GXXINC/cstdio" 2>/dev/null | head -8 | sed 's/^/  /'
echo "  --- the lines around cstdlib's inclusion of stdlib.h ---"
grep -n 'include.*stdlib\.h' "$GXXINC/cstdlib" 2>/dev/null | sed 's/^/  /'

echo
echo "=== which stdlib.h wins for a direct include, with the overlay? ==="
echo '#include <stdlib.h>' > /tmp/sd.cpp
"$CXX" -I"$SRF/usr/include" -I"$SRF/usr/include/aarch64-linux-gnu" -E -H /tmp/sd.cpp >/dev/null 2>/tmp/h3.txt
grep -E 'stdlib\.h' /tmp/h3.txt | head -3 | sed 's/^/  /'

echo
echo "=== the redirect header ==="
mkdir -p "$INC"
cat > "$INC/ekho-cross-stat.h" <<'EOF'
/* ekho-cross-stat.h -- forced include for the aarch64 cross build.
 *
 * The Arm 10.3 toolchain's sysroot is built on glibc 2.33; the board runs 2.31.
 * glibc 2.33 is where stat(), fstat() and lstat() stopped being macros over
 * __xstat() and became exported symbols in their own right. Compiling against
 * 2.33's headers therefore emits stat@GLIBC_2.33, and a 2.31 libc has no such
 * version entry at all -- the program does not start.
 *
 * Every aarch64 glibc since the port began, 2.17 upwards, exports
 * __xstat/__fxstat/__lxstat at GLIBC_2.17, and the board's does too. So point
 * the calls at those instead. aarch64 has only ever had one struct stat layout,
 * so the leading version argument is unused and 0 is correct.
 *
 * This is deliberately a build-side shim and not a patch to the tree: it is a
 * property of this toolchain and this target, not of ekho. The general fix is a
 * sysroot built for the target's glibc; this is the narrow version of it.
 */
#ifndef EKHO_CROSS_STAT_H
#define EKHO_CROSS_STAT_H

#include <sys/stat.h>
#include <sys/types.h>

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
echo "  $INC/ekho-cross-stat.h ($(stat -c%s "$INC/ekho-cross-stat.h") bytes)"

echo
echo "=== does it survive the real headers? (no overlay, forced include) ==="
P=/tmp/shimprobe; rm -rf $P; mkdir -p $P
cat > $P/p.c <<'EOF'
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
int main(int argc, char **argv) {
    struct stat st;
    int fd = open(argv[0], O_RDONLY);
    if (fd >= 0) fstat(fd, &st);
    if (stat(argv[0], &st) == 0) printf("%ld\n", (long)st.st_size);
    (void)argc; return 0;
}
EOF
cat > $P/p.cpp <<'EOF'
#include <filesystem>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <sys/stat.h>
int main(int argc, char **argv) {
    struct stat st;
    bool e = std::filesystem::exists(argv[0]);
    int fd = open(argv[0], O_RDONLY);
    if (fd >= 0) fstat(fd, &st);
    stat(argv[0], &st);
    std::thread t([]{});
    t.join();
    std::printf("%d %d\n", (int)e, (int)st.st_size);
    return 0;
}
EOF

report() {  # report <label> <ext> <compiler> <flags...>
    local label=$1 ext=$2 comp=$3; shift 3
    local out=$P/o; rm -f $out
    "$comp" "$@" $P/p.$ext -o $out -lpthread -ldl -lm 2>$P/err
    if [ ! -f $out ]; then printf '  %-34s BUILD FAILED: %s\n' "$label" "$(head -1 $P/err)"; return; fi
    local max syms
    max=$("$READELF" -V $out 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
    syms=$("$READELF" -sW --dyn-syms $out 2>/dev/null | grep -oE '[a-z_]+@GLIBC_2\.[0-9]+' | grep -E 'stat' | sort -u | tr '\n' ' ')
    printf '  %-34s max=%-13s %s\n' "$label" "$max" "$syms"
}

echo "  --- C, toolchain headers ---"
report "C   -O0  no shim"  c   "$CC"  -O0
report "C   -O0  shim"     c   "$CC"  -O0 -include "$INC/ekho-cross-stat.h"
report "C   -O2  shim"     c   "$CC"  -O2 -include "$INC/ekho-cross-stat.h"
echo "  --- C++ with <filesystem> + <thread> + <cstdlib>, toolchain headers ---"
report "C++ -O2  no shim"  cpp "$CXX" -std=gnu++17 -O2
report "C++ -O2  shim"     cpp "$CXX" -std=gnu++17 -O2 -include "$INC/ekho-cross-stat.h"
report "C++ -O0  shim"     cpp "$CXX" -std=gnu++17 -O0 -include "$INC/ekho-cross-stat.h"
report "C++ -O2  shim+overlay" cpp "$CXX" -std=gnu++17 -O2 \
       -include "$INC/ekho-cross-stat.h" -I"$SRF/usr/include" -I"$SRF/usr/include/aarch64-linux-gnu"

echo
echo "  board glibc is $BOARD_GLIBC_MAX -- anything above that must not appear"
