#!/bin/bash
# ASCII only. Facts needed before writing the cross-build: toolchain layout,
# host tools, and whether the box can fetch sources.
TC=/d/yzd/tools/cross/gcc-arm-10.3-2021.07-mingw-w64-i686-aarch64-none-linux-gnu
export PATH="$TC/bin:/d/yzd/MSYS2/mingw64/bin:/usr/bin:$PATH"

echo "=== [1] compiler ==="
aarch64-none-linux-gnu-gcc.exe --version | head -1
aarch64-none-linux-gnu-g++.exe --version | head -1

echo
echo "=== [2] where the toolchain sysroot actually keeps its libs ==="
find "$TC/aarch64-none-linux-gnu" -maxdepth 4 -name 'libc.so.6' -o -maxdepth 4 -name 'libm.so.6' \
     -o -maxdepth 4 -name 'libstdc++.so.6' -o -maxdepth 4 -name 'libgcc_s.so.1' 2>/dev/null | sed 's/^/  /'
echo "  --- sysroot top level ---"
ls -1 "$TC/aarch64-none-linux-gnu/" 2>/dev/null | sed 's/^/    /'
echo "  --- libc/ ---"
ls -1 "$TC/aarch64-none-linux-gnu/libc/" 2>/dev/null | sed 's/^/    /'

echo
echo "=== [3] host build tools ==="
for t in cmake ninja make gcc g++ autoconf automake libtoolize pkg-config perl \
         meson nasm yasm git curl wget tar bsdtar python3; do
  printf '  %-12s %s\n' "$t" "$(command -v $t 2>/dev/null || echo MISSING)"
done
cmake --version 2>/dev/null | head -1
ninja --version 2>/dev/null

echo
echo "=== [4] a hello-world round trip through the toolchain ==="
D=/tmp/xtest
rm -rf "$D"; mkdir -p "$D"
cat > "$D/h.c" <<'EOF'
#include <stdio.h>
#include <math.h>
#include <pthread.h>
int main(void) { printf("hi %f\n", sqrt(2.0)); return 0; }
EOF
aarch64-none-linux-gnu-gcc.exe -O2 "$D/h.c" -o "$D/h" -lm -lpthread 2>&1 | head -10
if [ -s "$D/h" ]; then
  echo "  linked: $(stat -c%s "$D/h") bytes"
  "$TC/bin/aarch64-none-linux-gnu-readelf.exe" -h "$D/h" | grep -E 'Class:|Machine:|Type:'
  echo "  --- DT_NEEDED ---"
  "$TC/bin/aarch64-none-linux-gnu-readelf.exe" -d "$D/h" | grep -o 'Shared library: *\[[^]]*\]' | sed 's/^/    /'
  echo "  --- symbol versions wanted ---"
  "$TC/bin/aarch64-none-linux-gnu-readelf.exe" -V "$D/h" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -3 | sed 's/^/    /'
else
  echo "  LINK FAILED"
fi

echo
echo "=== [5] c++ hello-world (libstdc++ reachable?) ==="
cat > "$D/h.cpp" <<'EOF'
#include <string>
#include <iostream>
int main() { std::string s = "ok"; std::cout << s << std::endl; return 0; }
EOF
aarch64-none-linux-gnu-g++.exe -O2 -std=c++17 "$D/h.cpp" -o "$D/hpp" 2>&1 | head -10
if [ -s "$D/hpp" ]; then
  echo "  linked: $(stat -c%s "$D/hpp") bytes"
  "$TC/bin/aarch64-none-linux-gnu-readelf.exe" -V "$D/hpp" 2>/dev/null \
    | grep -oE '(GLIBCXX|CXXABI|GLIBC)_[0-9.]+' | sort -uV | tail -5 | sed 's/^/    /'
else
  echo "  LINK FAILED"
fi

echo
echo "=== [6] can the box fetch sources? ==="
for U in https://github.com/libssndfile https://github.com/libsndfile/libsndfile/releases/download/1.2.2/libsndfile-1.2.2.tar.xz; do
  printf '  %-70s ' "$U"
  code=$(curl -sSL -o /tmp/dl.probe -w '%{http_code}' --max-time 40 "$U" 2>/dev/null)
  echo "http=$code bytes=$(stat -c%s /tmp/dl.probe 2>/dev/null || echo 0)"
done
echo "  proxy env: HTTP_PROXY=${HTTP_PROXY:-unset} HTTPS_PROXY=${HTTPS_PROXY:-unset}"

echo
echo "=== [7] what is already staged ==="
for d in /d/yzd/dl /d/yzd/ekho_patched /d/yzd/ekho_out /d/yzd/ekho_inst /d/yzd/share; do
  printf '  %-22s %s\n' "$d" "$(du -sh $d 2>/dev/null | cut -f1)"
done
ls -1 /d/yzd/dl 2>/dev/null | head -20 | sed 's/^/    /'
