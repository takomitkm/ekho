#!/bin/bash
# ASCII only. Gate 3 says the binaries need GLIBC_2.33 while the board has 2.31.
# Which symbol is it, and what glibc is the toolchain's sysroot actually made of?
. /c/Users/YiZhidi/x-common.sh
set -u
S=$XROOT/src/ekho-linux
B=$XROOT/kit-ekho/bin

echo "=== the sysroot's own glibc ==="
for f in libc.so.6 libpthread.so.0 libdl.so.2 libm.so.6 librt.so.1; do
    p=$(ls "$TC/aarch64-none-linux-gnu/libc/lib64/$f" 2>/dev/null)
    [ -n "$p" ] || continue
    printf '  %-16s %s\n' "$f" "$(strings "$p" 2>/dev/null | grep -m1 -E 'GNU C Library.*release version' || echo '(no version string)')"
done
echo "  --- and the lib64 stubs the linker actually picks up ---"
for f in libpthread.so libdl.so libm.so libc.so; do
    p=$(ls "$TC/aarch64-none-linux-gnu/libc/usr/lib64/$f" 2>/dev/null)
    [ -n "$p" ] || continue
    printf '  %-16s %8s bytes  %s\n' "$f" "$(stat -c%s "$p")" "$(strings "$p" 2>/dev/null | grep -m1 -E 'GNU C Library.*release version' | cut -c1-60)"
done

echo
echo "=== which symbols carry a GLIBC_2.3x version need, in the unstripped build ==="
for b in ekho ekho-stream ekho-server; do
    echo "  --- $b ---"
    "$READELF" -sW --dyn-syms "$S/$b" 2>/dev/null | grep -oE '[A-Za-z_][A-Za-z0-9_]*@GLIBC_[0-9.]+' | sort -u \
        | awk -F'@' '{print $2}' | sort -uV | tail -4 | sed 's/^/      need: /'
    "$READELF" -sW --dyn-syms "$S/$b" 2>/dev/null | grep -oE '[A-Za-z_][A-Za-z0-9_]*@GLIBC_2\.(3[0-9]|[4-9][0-9])[0-9.]*' | sort -u | sed 's/^/      /'
done

echo
echo "=== does the same symbol appear in a plain hello world with this toolchain? ==="
D=/tmp/glibcprobe; rm -rf $D; mkdir -p $D
cat > $D/h.c <<'EOF'
#include <stdio.h>
int main(void){ printf("hi\n"); return 0; }
EOF
"$CC" $D/h.c -o $D/h -lpthread -ldl -lm
echo "  C hello world: max $( "$READELF" -V $D/h 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)"
cat > $D/h.cpp <<'EOF'
#include <filesystem>
#include <thread>
#include <cstdio>
int main(){ std::printf("%d\n", (int)std::filesystem::exists("/")); std::thread t([]{});
            t.join(); return 0; }
EOF
"$CXX" -std=gnu++17 $D/h.cpp -o $D/hpp -lpthread -ldl -lm 2>/dev/null
echo "  C++ <filesystem>+<thread>: max $( "$READELF" -V $D/hpp 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)"
"$READELF" -sW --dyn-syms $D/hpp 2>/dev/null | grep -oE '[A-Za-z_][A-Za-z0-9_]*@GLIBC_2\.(3[0-9]|[4-9][0-9])[0-9.]*' | sort -u | sed 's/^/      /'
