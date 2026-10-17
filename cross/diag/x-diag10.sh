#!/bin/bash
# ASCII only. The suspected trigger: DictItem has a user-provided ctor AND dtor,
# so a 65536-element member array `DictItem mDictItemArray[65536] = {}` means
# 65536 constructor calls and 65536 destructor calls inside every Dict, and cc1plus
# is a 32-bit host binary. Test each half of that separately.
. /c/Users/YiZhidi/x-common.sh
set -u
cd "$XROOT/src/ekho-linux" || exit 1
D=/tmp/xice; mkdir -p $D
H=src/ekho_dict.h
cp -f "$H" $D/h.orig

INC=(-std=gnu++17 -pthread -DHAVE_CONFIG_H -I. -I./src
     -I"$XROOT/kit-sndfile/include" -I"$XROOT/kit-espeak/include"
     -I./third_party/sonic -I./third_party/alsa/include
     -I/d/yzd/MSYS2/mingw64/include/utf8cpp
     -DEKHO_DATA_PATH='"/d/yzd/ekho-x/kit-ekho/share/ekho-data"')

try() {
    local label="$1"
    rm -f $D/$label.o
    "$CXX" "${INC[@]}" -O2 -c src/ekho_dict.cpp -o $D/$label.o >$D/$label.out 2>$D/$label.err
    local rc=$?
    printf '  %-46s rc=%-3s obj=%-10s err=%s\n' "$label" "$rc" \
        "$([ -f $D/$label.o ] && stat -c%s $D/$label.o || echo absent)" "$(stat -c%s $D/$label.err)"
}

echo "=== variants (each reverts the header afterwards) ==="
try "A"

sed -i 's/mDictItemArray\[65536\] = {}/mDictItemArray[65536]/' "$H"
try "B_nobrace_16384"
cp -f $D/h.orig "$H"

sed -i 's/SYMBOL_ARRAY_SIZE = 16384/SYMBOL_ARRAY_SIZE = 8000/' "$H"
try "C_8000_with_braces"
cp -f $D/h.orig "$H"

sed -i -e 's/mDictItemArray\[65536\] = {}/mDictItemArray[65536]/' \
       -e 's/SYMBOL_ARRAY_SIZE = 16384/SYMBOL_ARRAY_SIZE = 8000/' "$H"
try "D_8000_nobrace"
cp -f $D/h.orig "$H"

echo
echo "=== header restored? ==="
md5sum $D/h.orig "$H" | sed 's/^/  /'

echo
echo "=== peak memory of cc1plus during variant A (OOM, or a genuine ICE?) ==="
( "$CXX" "${INC[@]}" -O2 -c src/ekho_dict.cpp -o $D/A2.o >/dev/null 2>&1 ) &
bg=$!
peak=0
samples=0
while kill -0 $bg 2>/dev/null; do
    line=$(cmd //c "tasklist /FI \"IMAGENAME eq cc1plus.exe\" /FO CSV /NH" 2>/dev/null | head -1)
    kb=$(echo "$line" | tr -d '"' | awk -F, '{gsub(/[^0-9]/,"",$5); print $5}')
    if [ -n "$kb" ] && [ "$kb" -gt "$peak" ] 2>/dev/null; then peak=$kb; fi
    samples=$((samples+1))
    sleep 1
done
wait $bg; rc=$?
echo "  rc=$rc  samples=$samples  peak cc1plus working set: $((peak/1024)) MB"
echo "  (a 32-bit process tops out near 2048 MB; if peak is pinned near that, it is OOM)"

echo
echo "=== how big is one Dict, in bytes, according to the compiler ==="
cat > $D/sz.cpp <<'EOF'
#include "ekho_dict.h"
#include <cstdio>
int main() { printf("sizeof(Dict)=%zu sizeof(DictItem)=%zu sizeof(PhoneticSymbol)=%zu\n",
                    sizeof(ekho::Dict), sizeof(ekho::DictItem), sizeof(ekho::PhoneticSymbol)); return 0; }
EOF
"$CXX" "${INC[@]}" -O0 -fsyntax-only $D/sz.cpp > $D/sz.err 2>&1 && echo "  header parses standalone" || head -3 $D/sz.err | sed 's/^/    /'
grep -nE 'class Dict|typedef struct|using namespace' src/ekho.h src/ekho_typedef.h 2>/dev/null | head -5 | sed 's/^/  /'
