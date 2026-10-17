#!/bin/bash
# ASCII only. -O0 works, -O1/-O2/-Os all die silently, and -g is present in every
# case. -g plus optimisation is what turns on var-tracking, which is the most
# memory-hungry thing the compiler does -- and cc1plus here is a 32-bit binary.
. /c/Users/YiZhidi/x-common.sh
set -u
cd "$XROOT/src/ekho-linux" || exit 1
D=/tmp/xice

echo "=== stderr from the dump run (255 bytes, unread so far) ==="
cat $D/de 2>/dev/null | sed 's/^/  /'

echo
echo "=== is the host compiler 32-bit? ==="
CC1=$(dirname "$CXX")/../libexec/gcc/aarch64-none-linux-gnu/10.3.1/cc1plus.exe
ls -l "$CC1" 2>/dev/null | sed 's/^/  /'
"$OBJDUMP" -f "$CC1" 2>/dev/null | grep -E 'file format|architecture' | sed 's/^/  /'

echo
echo "=== memory on this box ==="
free -m 2>/dev/null | sed 's/^/  /' || wmic OS get FreePhysicalMemory 2>/dev/null | sed 's/^/  /'

INC=(-std=gnu++17 -pthread -DHAVE_CONFIG_H -I. -I./src
     -I"$XROOT/kit-sndfile/include" -I"$XROOT/kit-espeak/include"
     -I./third_party/sonic -I./third_party/alsa/include
     -I/d/yzd/MSYS2/mingw64/include/utf8cpp
     -DEKHO_DATA_PATH='"/d/yzd/ekho-x/kit-ekho/share/ekho-data"')

try() { local desc=$1; shift; "$@" >$D/o 2>$D/e; printf '  %-40s rc=%-3s err=%s\n' "$desc" "$?" "$(stat -c%s $D/e)"; head -3 $D/e | sed 's/^/      /'; }

echo
echo "=== var-tracking, tested directly ==="
try "-O2 -g"                                  "$CXX" "${INC[@]}" -O2 -g  -c src/ekho_dict.cpp -o $D/t1.o
try "-O2 -g0   (no debug info)"               "$CXX" "${INC[@]}" -O2 -g0 -c src/ekho_dict.cpp -o $D/t2.o
try "-O2        (no -g at all)"               "$CXX" "${INC[@]}" -O2     -c src/ekho_dict.cpp -o $D/t3.o
try "-O1 -g"                                  "$CXX" "${INC[@]}" -O1 -g  -c src/ekho_dict.cpp -o $D/t4.o
try "-O0 -g"                                  "$CXX" "${INC[@]}" -O0 -g  -c src/ekho_dict.cpp -o $D/t5.o
try "-O2 -g -fno-var-tracking"                "$CXX" "${INC[@]}" -O2 -g -fno-var-tracking -c src/ekho_dict.cpp -o $D/t6.o
try "-O2 -g -fno-var-tracking-assignments"    "$CXX" "${INC[@]}" -O2 -g -fno-var-tracking-assignments -c src/ekho_dict.cpp -o $D/t7.o

echo
echo "=== which of those produced an object ==="
for f in t1 t2 t3 t4 t5 t6 t7; do
    [ -f $D/$f.o ] && printf '  %-4s %s bytes\n' "$f" "$(stat -c%s $D/$f.o)" || printf '  %-4s absent\n' "$f"
done
