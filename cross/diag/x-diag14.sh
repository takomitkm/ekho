#!/bin/bash
# ASCII only. The exact recipe fails 100% and a hand-built command with the same
# inputs succeeds. Three things differ: -Wall, the -MD/-MP/-MF group, and a
# relative -o. 2x2x2, known-good variant first so an ordering effect would show.
. /c/Users/YiZhidi/x-common.sh
set -u
cd "$XROOT/src/ekho-linux" || exit 1
D=/tmp/xice; mkdir -p $D

BASE=(-DHAVE_CONFIG_H -I. -I./src
      -ID:/yzd/ekho-x/kit-sndfile/include -ID:/yzd/ekho-x/kit-espeak/include
      -I./third_party/sonic -I./third_party/alsa/include
      -I/d/yzd/MSYS2/mingw64/include/utf8cpp)
OPT=(-O2 -g)
DEF=(-DEKHO_DATA_PATH='"/d/yzd/ekho-x/kit-ekho/share/ekho-data"')
PTH=(-pthread -std=gnu++17)
WALL=(-Wall)
DEP=(-MT src/libekho_a-ekho_dict.o -MD -MP -MF src/.deps/libekho_a-ekho_dict.Tpo)

run() {   # run <desc> <dep:0|1> <wall:0|1> <relo:0|1>
    local desc=$1 dep=$2 wall=$3 relo=$4
    local out; [ "$relo" = 1 ] && out=src/libekho_a-ekho_dict.o || out=$D/out.o
    local args=("${BASE[@]}" "${OPT[@]}" "${DEF[@]}" "${PTH[@]}")
    [ "$wall" = 1 ] && args+=("${WALL[@]}")
    [ "$dep"  = 1 ] && args+=("${DEP[@]}")
    rm -f "$out"
    "$CXX" "${args[@]}" -c -o "$out" src/ekho_dict.cpp >$D/o 2>$D/e
    local rc=$?
    printf '  %-30s rc=%-3s obj=%-9s err=%s\n' "$desc" "$rc" \
        "$([ -f "$out" ] && stat -c%s "$out" || echo absent)" "$(stat -c%s $D/e)"
    head -4 $D/e | sed 's/^/        /'
    rm -f "$out"
}

echo "=== 2x2x2 over dep-flags / -Wall / relative -o ==="
run "dep=0 wall=0 rel=0 (hand-built)" 0 0 0
run "dep=0 wall=0 rel=1"              0 0 1
run "dep=0 wall=1 rel=0"              0 1 0
run "dep=1 wall=0 rel=0"              1 0 0
run "dep=0 wall=1 rel=1"              0 1 1
run "dep=1 wall=0 rel=1"              1 0 1
run "dep=1 wall=1 rel=0"              1 1 0
run "dep=1 wall=1 rel=1 (the recipe)" 1 1 1

echo
echo "=== and with -O0, to see whether optimisation is involved at all ==="
run_bare() { local desc=$1; shift
    rm -f $D/out.o; "$CXX" "${BASE[@]}" "${DEF[@]}" "${PTH[@]}" "$@" -c -o $D/out.o src/ekho_dict.cpp >$D/o 2>$D/e
    printf '  %-30s rc=%-3s obj=%-9s err=%s\n' "$desc" "$?" "$([ -f $D/out.o ] && stat -c%s $D/out.o || echo absent)" "$(stat -c%s $D/e)"
    rm -f $D/out.o; }
run_bare "O0 + -Wall + dep" -O0 -g -Wall -MT x -MD -MP -MF src/.deps/x.Tpo
run_bare "O1 + -Wall + dep" -O1 -g -Wall -MT x -MD -MP -MF src/.deps/x.Tpo
run_bare "O2 + -Wall + dep" -O2 -g -Wall -MT x -MD -MP -MF src/.deps/x.Tpo
run_bare "O2 -g -Wall, no dep" -O2 -g -Wall
run_bare "O2 -g, dep, no -Wall" -O2 -g -MT x -MD -MP -MF src/.deps/x.Tpo
