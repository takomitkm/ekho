#!/bin/bash
# ASCII only. The overlay is in place now (sys/stat.h present). But glibc 2.31's
# <sys/stat.h> only reaches __xstat through an inline, and that inline is gated
# on __USE_EXTERN_INLINES, which needs an optimising compiler. ekho builds at
# -O2, so test at -O2, and in C++ because that is what ekho is.
. /c/Users/YiZhidi/x-common.sh
set -u
KIT=$XROOT/kit-sysroot-focal
P=/tmp/statprobe3; rm -rf $P; mkdir -p $P
OV="-I$KIT/usr/include -I$KIT/usr/include/aarch64-linux-gnu"

cat > $P/p.c <<'EOF'
#include <sys/stat.h>
#include <sys/types.h>
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
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <cstdio>
int main(int argc, char **argv) {
    struct stat st;
    int fd = open(argv[0], O_RDONLY);
    if (fd >= 0) fstat(fd, &st);
    if (stat(argv[0], &st) == 0) std::printf("%ld\n", (long)st.st_size);
    (void)argc; return 0;
}
EOF

echo "=== which <sys/stat.h> does the overlay actually pick? ==="
"$CC" $OV -M $P/p.c 2>/dev/null | tr ' ' '\n' | grep 'sys/stat\.h' | sort -u | sed 's/^/  with overlay:    /'
"$CC"      -M $P/p.c 2>/dev/null | tr ' ' '\n' | grep 'sys/stat\.h' | sort -u | sed 's/^/  without overlay: /'

echo
echo "=== is __USE_EXTERN_INLINES on? ==="
for spec in "CC -O0" "CC -O2" "CXX -O0" "CXX -O2"; do
    set -- $spec
    if [ "$1" = CC ]; then comp="$CC"; lang=c; else comp="$CXX"; lang=c++; fi
    n=$(echo '#include <sys/stat.h>' | "$comp" $OV "$2" -dM -E -x "$lang" - 2>/dev/null | grep -c '__USE_EXTERN_INLINES')
    printf '  %-10s %s: %s\n' "$1" "$2" "$([ "$n" -gt 0 ] && echo defined || echo 'not defined')"
done

echo
echo "=== stat and fstat symbol versions ==="
probe() {   # probe <label> <ext> <compiler> <flags...>
    local label=$1 ext=$2 comp=$3; shift 3
    local out=$P/o
    rm -f $out
    "$comp" "$@" $P/p.$ext -o $out -lpthread -ldl -lm 2>$P/err
    if [ ! -f $out ]; then printf '  %-30s BUILD FAILED: %s\n' "$label" "$(head -1 $P/err)"; return; fi
    local max syms
    max=$("$READELF" -V $out 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
    syms=$("$READELF" -sW --dyn-syms $out 2>/dev/null | grep -oE '[a-z_]+@GLIBC_2\.[0-9]+' | grep -E '(stat|fstat)' | sort -u | tr '\n' ' ')
    printf '  %-30s max=%-13s %s\n' "$label" "$max" "$syms"
}

probe "C   -O0  toolchain hdrs" c   "$CC"  -O0
probe "C   -O0  focal overlay"  c   "$CC"  -O0 $OV
probe "C   -O2  focal overlay"  c   "$CC"  -O2 $OV
probe "C   -O2  overlay+inline" c   "$CC"  -O2 $OV -D__USE_EXTERN_INLINES=1
probe "C   -O0  overlay+inline" c   "$CC"  -O0 $OV -D__USE_EXTERN_INLINES=1
probe "C++ -O2  toolchain hdrs" cpp "$CXX" -O2
probe "C++ -O2  focal overlay"  cpp "$CXX" -O2 $OV
probe "C++ -O2  overlay+inline" cpp "$CXX" -O2 $OV -D__USE_EXTERN_INLINES=1
probe "C++ -O0  overlay+inline" cpp "$CXX" -O0 $OV -D__USE_EXTERN_INLINES=1

echo
echo "  board glibc is $BOARD_GLIBC_MAX -- anything above that must not appear"
