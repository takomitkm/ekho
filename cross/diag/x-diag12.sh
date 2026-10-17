#!/bin/bash
# ASCII only. Disk, memory, and is the -O2 failure reproducible? No du over the
# whole drive this time -- that is what hung.
. /c/Users/YiZhidi/x-common.sh
set -u
cd "$XROOT/src/ekho-linux" || exit 1
D=/tmp/xice; mkdir -p $D

echo "=== disk (df, not du) ==="
df -h 2>/dev/null | sed 's/^/  /'

echo
echo "=== memory ==="
head -4 /proc/meminfo 2>/dev/null | sed 's/^/  /' || echo "  no /proc/meminfo"

echo
echo "=== the workspace, one level only ==="
ls -1 "$XROOT" 2>/dev/null | sed 's/^/  /'

INC=(-std=gnu++17 -pthread -DHAVE_CONFIG_H -I. -I./src
     -I"$XROOT/kit-sndfile/include" -I"$XROOT/kit-espeak/include"
     -I./third_party/sonic -I./third_party/alsa/include
     -I/d/yzd/MSYS2/mingw64/include/utf8cpp
     -DEKHO_DATA_PATH='"/d/yzd/ekho-x/kit-ekho/share/ekho-data"')

echo
echo "=== -O2 -g, five times serially ==="
for i in 1 2 3 4 5; do
    rm -f $D/r-$i.o
    "$CXX" "${INC[@]}" -O2 -g -c src/ekho_dict.cpp -o $D/r-$i.o >$D/r-$i.out 2>$D/r-$i.err
    printf '  run %d: rc=%-3s obj=%-9s err=%s\n' "$i" "$?" \
        "$([ -f $D/r-$i.o ] && stat -c%s $D/r-$i.o || echo absent)" "$(stat -c%s $D/r-$i.err)"
    rm -f $D/r-$i.o
done

echo
echo "=== eight at once, as make -j8 does ==="
for i in 1 2 3 4 5 6 7 8; do
    ( "$CXX" "${INC[@]}" -O2 -g -c src/ekho_dict.cpp -o $D/p-$i.o >$D/p-$i.out 2>$D/p-$i.err; echo $? > $D/p-$i.rc ) &
done
wait
for i in 1 2 3 4 5 6 7 8; do
    printf '  parallel %d: rc=%s obj=%-9s\n' "$i" "$(cat $D/p-$i.rc 2>/dev/null)" \
        "$([ -f $D/p-$i.o ] && stat -c%s $D/p-$i.o || echo absent)"
    rm -f $D/p-$i.o
done

echo
echo "=== disk again, after ==="
df -h /d /c 2>/dev/null | sed 's/^/  /'
