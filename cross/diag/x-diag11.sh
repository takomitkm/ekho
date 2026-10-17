#!/bin/bash
# ASCII only. The same -O2 compile failed twice and then succeeded once, with no
# message either way. That is not a compiler bug, that is a resource running out.
# Check disk and memory, then prove it by repetition.
. /c/Users/YiZhidi/x-common.sh
set -u
cd "$XROOT/src/ekho-linux" || exit 1
D=/tmp/xice; mkdir -p $D

echo "=== disk ==="
df -h 2>/dev/null | sed 's/^/  /'

echo
echo "=== memory ==="
head -5 /proc/meminfo 2>/dev/null | sed 's/^/  /' || echo "  no /proc/meminfo"

echo
echo "=== how much is under the workspace ==="
du -sh /d/yzd/ekho-x 2>/dev/null | sed 's/^/  /'
for d in /d/yzd/ekho-x/*/; do printf '  %-30s %s\n' "$(basename "$d")" "$(du -sh "$d" 2>/dev/null | cut -f1)"; done
echo "  --- the biggest single directories on D: ---"
du -sh /d/yzd/* 2>/dev/null | sort -h | tail -8 | sed 's/^/    /'

echo
echo "=== repeat the -O2 compile five times, serially ==="
INC=(-std=gnu++17 -pthread -DHAVE_CONFIG_H -I. -I./src
     -I"$XROOT/kit-sndfile/include" -I"$XROOT/kit-espeak/include"
     -I./third_party/sonic -I./third_party/alsa/include
     -I/d/yzd/MSYS2/mingw64/include/utf8cpp
     -DEKHO_DATA_PATH='"/d/yzd/ekho-x/kit-ekho/share/ekho-data"')
for i in 1 2 3 4 5; do
    rm -f $D/r.o
    "$CXX" "${INC[@]}" -O2 -g -c src/ekho_dict.cpp -o $D/r.o >$D/r.out 2>$D/r.err
    printf '  run %d: rc=%-3s obj=%-10s err=%s\n' "$i" "$?" \
        "$([ -f $D/r.o ] && stat -c%s $D/r.o || echo absent)" "$(stat -c%s $D/r.err)"
done

echo
echo "=== now with eight at once, the way make -j8 does it ==="
pids=""
for i in 1 2 3 4 5 6 7 8; do
    ( "$CXX" "${INC[@]}" -O2 -g -c src/ekho_dict.cpp -o $D/p$i.o >$D/p$i.out 2>$D/p$i.err; echo $? > $D/p$i.rc ) &
    pids="$pids $!"
done
wait
for i in 1 2 3 4 5 6 7 8; do
    printf '  parallel %d: rc=%s obj=%s\n' "$i" "$(cat $D/p$i.rc)" \
        "$([ -f $D/p$i.o ] && stat -c%s $D/p$i.o || echo absent)"
done

rm -f $D/*.o $D/*.s 2>/dev/null
echo
echo "=== free space after clearing the scratch ==="
df -h 2>/dev/null | sed 's/^/  /'
