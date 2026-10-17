#!/bin/bash
# ASCII only. -O2 dies with no message; -O0 is fine. Narrow it to a pass.
. /c/Users/YiZhidi/x-common.sh
set -u
cd "$XROOT/src/ekho-linux" || exit 1

D=/tmp/xice; rm -rf $D; mkdir -p $D

INC=(-std=gnu++17 -g -pthread -DHAVE_CONFIG_H -I. -I./src
     -I"$XROOT/kit-sndfile/include" -I"$XROOT/kit-espeak/include"
     -I./third_party/sonic -I./third_party/alsa/include
     -I/d/yzd/MSYS2/mingw64/include/utf8cpp
     -DEKHO_DATA_PATH='"/d/yzd/ekho-x/kit-ekho/share/ekho-data"')

try() { local desc=$1; shift; "$@" >$D/o 2>$D/e; printf '  %-34s rc=%-3s err=%s\n' "$desc" "$?" "$(stat -c%s $D/e)"; head -3 $D/e | sed 's/^/      /'; }

echo "=== which optimisation level survives ==="
try "-O0"                    "$CXX" "${INC[@]}" -O0 -c src/ekho_dict.cpp -o $D/a.o
try "-O1"                    "$CXX" "${INC[@]}" -O1 -c src/ekho_dict.cpp -o $D/b.o
try "-O2"                    "$CXX" "${INC[@]}" -O2 -c src/ekho_dict.cpp -o $D/c.o
try "-Os"                    "$CXX" "${INC[@]}" -Os -c src/ekho_dict.cpp -o $D/d.o
try "-O2 -fno-inline"        "$CXX" "${INC[@]}" -O2 -fno-inline -c src/ekho_dict.cpp -o $D/e.o
try "-O2 -fno-tree-vrp"      "$CXX" "${INC[@]}" -O2 -fno-tree-vrp -c src/ekho_dict.cpp -o $D/f.o
try "-O2 -fno-ipa-icf"       "$CXX" "${INC[@]}" -O2 -fno-ipa-icf -c src/ekho_dict.cpp -o $D/g.o
try "-O2 -fno-ipa-cp"        "$CXX" "${INC[@]}" -O2 -fno-ipa-cp -c src/ekho_dict.cpp -o $D/h.o
try "-O2 -fno-tree-dominator-opts" "$CXX" "${INC[@]}" -O2 -fno-tree-dominator-opts -c src/ekho_dict.cpp -o $D/i.o
try "-O2 -fno-gcse"          "$CXX" "${INC[@]}" -O2 -fno-gcse -c src/ekho_dict.cpp -o $D/j.o
try "-O2 -fno-tree-sra"      "$CXX" "${INC[@]}" -O2 -fno-tree-sra -c src/ekho_dict.cpp -o $D/k.o
try "-O2 -fno-strict-aliasing" "$CXX" "${INC[@]}" -O2 -fno-strict-aliasing -c src/ekho_dict.cpp -o $D/l.o

echo
echo "=== -v, to see how far the driver gets before cc1plus vanishes ==="
"$CXX" "${INC[@]}" -O2 -c src/ekho_dict.cpp -o $D/v.o -v > $D/vo 2> $D/ve
echo "  rc=$?  stderr $(stat -c%s $D/ve) bytes; last 12 lines:"
tail -12 $D/ve | sed 's/^/    /'

echo
echo "=== which pass was running: dump every tree pass and see where it stops ==="
cd $D || exit 1
"$CXX" "${INC[@]}" -O2 -fdump-tree-all -dumpbase db -c "$XROOT/src/ekho-linux/src/ekho_dict.cpp" -o $D/dump.o > $D/do 2> $D/de
echo "  rc=$?  stderr $(stat -c%s $D/de) bytes"
echo "  passes that produced a dump (the next one is the one that died):"
ls -1 $D/db.* 2>/dev/null | sed 's/.*\.\([0-9]*t\.[a-z_-]*\).*/    \1/' | tail -12
echo "  total dumps: $(ls -1 $D/db.* 2>/dev/null | wc -l)"
