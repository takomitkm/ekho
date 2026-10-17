#!/bin/bash
# ASCII only. kit-sysroot-focal: the headers for the glibc the board actually has.
#
# The Arm 10.3 toolchain ships a sysroot built on glibc 2.33. The board runs
# 2.31. That mismatch is invisible in a hello world -- it needs GLIBC_2.17 --
# and fatal in ekho, which calls stat() and fstat():
#
#     stat@GLIBC_2.33    fstat@GLIBC_2.33
#
# glibc 2.33 is where stat and fstat stopped being macros over __xstat and
# became exported functions in their own right. Compiling against 2.33 headers
# therefore emits a call to a symbol the board's libc has never heard of, and
# stage 3 of the L1 gates fails. The headers are what leak the newer ABI, so
# the headers are what gets pinned: focal's libc6-dev and linux-libc-dev, used
# as an include overlay in front of the toolchain's own sysroot. Linking still
# uses the toolchain's libs, which is fine -- __xstat's ABI has not moved.
. /c/Users/YiZhidi/x-common.sh
set -u

KIT=$XROOT/kit-sysroot-focal
DL=$XROOT/dl
BASE=http://mirrors.ustc.edu.cn/ubuntu-ports/pool/main

echo "=== kit-sysroot-focal -> $KIT ==="
kit_reset "$KIT"
mkdir -p "$KIT/usr" "$DL"

fetch() {   # fetch <pool-path-regex> <label>
    local rx=$1 label=$2 url file
    url=$(curl -fsS --max-time 60 "$BASE/g/glibc/" 2>/dev/null | grep -oE 'libc6-dev_2\.31-0ubuntu9[^"]*_arm64\.deb' | sort -uV | tail -1)
    [ -n "$url" ] || { echo "  no libc6-dev found in the glibc pool"; return 1; }
    file=$DL/$url
    if [ ! -f "$file" ]; then
        echo "  fetching $url"
        curl -fsS --max-time 300 -o "$file" "$BASE/g/glibc/$url" || return 1
    fi
    echo "  $label: $(basename "$file") ($(stat -c%s "$file") B)"
}

echo "--- what the pool offers ---"
curl -fsS --max-time 60 "$BASE/g/glibc/" 2>/dev/null \
    | grep -oE 'libc6-dev_2\.31[^"]*_arm64\.deb' | sort -uV | tail -5 | sed 's/^/    /'
curl -fsS --max-time 60 "$BASE/l/linux/" 2>/dev/null \
    | grep -oE 'linux-libc-dev_[^"]*_arm64\.deb' | sort -uV | tail -3 | sed 's/^/    /'

echo
echo "--- download ---"
LIBDEV=$(curl -fsS --max-time 60 "$BASE/g/glibc/" 2>/dev/null | grep -oE 'libc6-dev_2\.31-0ubuntu9[^"]*_arm64\.deb' | sort -uV | tail -1)
LINDEV=$(curl -fsS --max-time 60 "$BASE/l/linux/" 2>/dev/null | grep -oE 'linux-libc-dev_[0-9][^"]*_arm64\.deb' | sort -uV | tail -1)
for f in "$LIBDEV" "$LINDEV"; do
    [ -n "$f" ] || continue
    if [ ! -f "$DL/$f" ]; then
        echo "  fetching $f"
        curl -fsS --max-time 300 -o "$DL/$f" "$BASE/g/glibc/$f" 2>/dev/null \
          || curl -fsS --max-time 300 -o "$DL/$f" "$BASE/l/linux/$f" || echo "    FAILED $f"
    fi
    printf '  %-46s %s B\n' "$f" "$(stat -c%s "$DL/$f" 2>/dev/null)"
done

echo
echo "--- extract the usr/include trees ---"
T=/tmp/sysrootx; rm -rf $T; mkdir -p $T
for f in "$LIBDEV" "$LINDEV"; do
    [ -f "$DL/$f" ] || continue
    rm -rf $T/d; mkdir -p $T/d
    ( cd $T/d && bsdtar -xf "$DL/$f" 2>/dev/null )
    inner=$(ls $T/d/data.tar.* 2>/dev/null | head -1)
    [ -n "$inner" ] || { echo "  no data.tar in $f"; continue; }
    echo "  $f: $(bsdtar -tf "$inner" | grep -c '^\./usr/include') header entries"
    ( cd "$KIT" && bsdtar -xf "$inner" ./usr/include 2>/dev/null )
done
rm -rf $T

echo
echo "--- what arrived ---"
find "$KIT" -type f | wc -l | sed 's/^/  files: /'
ls -1 "$KIT/usr/include" 2>/dev/null | head -12 | sed 's/^/    /'
echo "  sys/stat.h:      $(test -f "$KIT/usr/include/sys/stat.h" && echo present || echo MISSING)"
echo "  bits/stat.h:     $(test -f "$KIT/usr/include/bits/stat.h" && echo present || echo '(in aarch64-linux-gnu/)')"
ls -1 "$KIT/usr/include/aarch64-linux-gnu/bits/stat.h" 2>/dev/null | sed 's/^/    /'
echo "  linux/stat.h:    $(test -f "$KIT/usr/include/linux/stat.h" && echo present || echo MISSING)"
echo "  asm/ (aarch64):  $(ls -1 "$KIT/usr/include/aarch64-linux-gnu/asm" 2>/dev/null | wc -l) files"

echo
echo "=== does the overlay actually change the ABI? ==="
P=/tmp/statprobe; rm -rf $P; mkdir -p $P
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
Overlay="-I$KIT/usr/include -I$KIT/usr/include/aarch64-linux-gnu"
echo "  --- without the overlay (toolchain headers, glibc 2.33) ---"
"$CC" $P/p.c -o $P/a -lpthread -ldl -lm 2>&1 | head -5 | sed 's/^/      /'
"$READELF" -sW --dyn-syms $P/a 2>/dev/null | grep -oE '[a-z]+@GLIBC_2\.[0-9]+' | sort -u | sed 's/^/      /'
echo "      max GLIBC: $("$READELF" -V $P/a 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)"
echo "  --- with the overlay (focal headers, glibc 2.31) ---"
"$CC" $Overlay $P/p.c -o $P/b -lpthread -ldl -lm 2>&1 | head -5 | sed 's/^/      /'
"$READELF" -sW --dyn-syms $P/b 2>/dev/null | grep -oE '[a-z]+@GLIBC_2\.[0-9]+' | sort -u | sed 's/^/      /'
echo "      max GLIBC: $("$READELF" -V $P/b 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)"
echo "  (the board is glibc $BOARD_GLIBC_MAX)"

echo
echo "=== is __xstat still linkable in the toolchain's libc? ==="
"$NM" -D --defined-only "$TC/aarch64-none-linux-gnu/libc/lib64/libc.so.6" 2>/dev/null | grep -E ' (__xstat|__fxstat|__lxstat)64?$' | sed 's/^/  /'

kit_finish "$KIT" sysroot-focal 2.31-0ubuntu9 \
  "libc6-dev + linux-libc-dev for arm64, USTC ubuntu-ports pool (focal, the board's glibc)" \
  "usr/include overlay: glibc 2.31 headers, so the toolchain's 2.33 headers cannot leak stat@GLIBC_2.33 into the link"
