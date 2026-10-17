#!/bin/bash
# ASCII only. The focal overlay extracted 499 of 524 header entries. Debian
# multiarch uses symlinks for /usr/include/sys and friends, and this is MSYS2,
# where symlinks cannot be created -- so the whole sys/ directory is missing and
# the overlay silently did nothing. Find every lost symlink and copy the real
# directory in instead.
. /c/Users/YiZhidi/x-common.sh
set -u
KIT=$XROOT/kit-sysroot-focal
DL=$XROOT/dl
DEB=$(ls -1 "$DL"/libc6-dev_2.31-0ubuntu9_arm64.deb 2>/dev/null | head -1)
[ -f "$DEB" ] || { echo "no deb"; exit 1; }

T=/tmp/sysrootx; rm -rf $T; mkdir -p $T
( cd $T && bsdtar -xf "$DEB" 2>/dev/null )
INNER=$(ls $T/data.tar.* 2>/dev/null | head -1)

echo "=== symlinks inside the package (these are what MSYS2 dropped) ==="
bsdtar -tvf "$INNER" 2>/dev/null | grep -E '\->' | grep 'usr/include' | head -20 | sed 's/^/  /'

echo
echo "=== what the kit currently has at the top level ==="
ls -1 "$KIT/usr/include" | head -40 | tr '\n' ' ' | sed 's/^/  /'
echo
echo "  aarch64-linux-gnu/sys/stat.h : $(test -f "$KIT/usr/include/aarch64-linux-gnu/sys/stat.h" && echo present || echo MISSING)"
echo "  aarch64-linux-gnu/bits/stat.h: $(test -f "$KIT/usr/include/aarch64-linux-gnu/bits/stat.h" && echo present || echo MISSING)"
echo "  aarch64-linux-gnu/ entries   : $(ls -1 "$KIT/usr/include/aarch64-linux-gnu" 2>/dev/null | tr '\n' ' ')"
echo "  aarch64-linux-gnu/sys files  : $(ls -1 "$KIT/usr/include/aarch64-linux-gnu/sys" 2>/dev/null | wc -l)"

echo
echo "=== repair: copy the symlink targets into place ==="
for name in sys gnu bits asm linux; do
    if [ -e "$KIT/usr/include/$name" ]; then
        echo "  $name: already present"
        continue
    fi
    real=$(ls -d "$KIT/usr/include"/*/"$name" 2>/dev/null | head -1)
    if [ -n "$real" ]; then
        cp -r "$real" "$KIT/usr/include/$name"
        echo "  $name: copied out of the multiarch dir, $(find "$KIT/usr/include/$name" -type f | wc -l) files"
    else
        echo "  $name: not in the package (toolchain's copy will be used)"
    fi
done

echo
echo "=== after repair ==="
echo "  sys/stat.h : $(test -f "$KIT/usr/include/sys/stat.h" && echo present || echo MISSING)"
echo "  files now  : $(find "$KIT" -type f | wc -l)"

echo
echo "=== does the overlay change the ABI now? ==="
P=/tmp/statprobe2; rm -rf $P; mkdir -p $P
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
OV="-I$KIT/usr/include -I$KIT/usr/include/aarch64-linux-gnu"
"$CC" $P/p.c -o $P/a -lpthread -ldl -lm 2>/dev/null
echo "  toolchain headers : max GLIBC $("$READELF" -V $P/a 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)"
"$CC" $OV $P/p.c -o $P/b -lpthread -ldl -lm 2>&1 | head -4 | sed 's/^/      /'
if [ -f $P/b ]; then
    echo "  focal headers     : max GLIBC $("$READELF" -V $P/b 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)"
    "$READELF" -sW --dyn-syms $P/b 2>/dev/null | grep -oE '[a-z_]+@GLIBC_2\.[0-9]+' | sort -u | sed 's/^/      /'
fi

echo
echo "=== is __xstat linkable from the toolchain libc? ==="
"$NM" -D --defined-only "$TC/aarch64-none-linux-gnu/libc/lib64/libc.so.6" 2>/dev/null | grep -E '(__xstat|__fxstat|__lxstat)' | sed 's/^/  /'
echo
echo "=== and the board? ==="
grep -cE '__xstat|__fxstat' /c/Users/YiZhidi/board-libs.txt 2>/dev/null | sed 's/^/  board-libs.txt mentions: /'
rm -rf $T
