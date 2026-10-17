#!/bin/bash
# ASCII only. -O0 always works and anything above it usually does not, silently.
# cc1plus here is a 32-bit host binary, so "usually" is what running out of a 2 GB
# address space looks like. The thing that eats it is almost certainly
# DictItem mDictItemArray[65536], where DictItem has a user-provided ctor AND
# dtor -- 65536 constructor calls and 65536 destructor calls in every Dict.
# Test: make DictItem trivial and see whether -O2 comes back.
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

c() { local lbl=$1; shift; rm -f $D/o.o
      "$CXX" "${INC[@]}" "$@" -c src/ekho_dict.cpp -o $D/o.o >$D/o 2>$D/e
      printf '  %-38s rc=%-3s obj=%-9s\n' "$lbl" "$?" "$([ -f $D/o.o ] && stat -c%s $D/o.o || echo absent)"
      rm -f $D/o.o; }

echo "=== how big is one Dict? (ask the compiler by making it fail) ==="
cat > $D/sz.cpp <<'EOF'
#include "ekho_dict.h"
template<int N> struct Show;
Show<sizeof(ekho::Dict)>*      probeDict;
Show<sizeof(ekho::DictItem)>*  probeItem;
Show<sizeof(ekho::PhoneticSymbol)>* probeSym;
EOF
"$CXX" "${INC[@]}" -O0 -c $D/sz.cpp -o $D/sz.o > $D/sz.out 2> $D/sz.err
grep -oE "Show<[0-9]+>" $D/sz.err | sort -u | sed 's/^/  sizeof from the error: /'

echo
echo "=== control: as committed ==="
c "control, -O2 (expect fail)" -O2 -g
c "control, -O0 (expect ok)"   -O0 -g

echo
echo "=== now make DictItem trivial (no user ctor, no user dtor) ==="
perl -0777 -pi -e 's/^  DictItem\(\) \{ wordList = 0; \}\r?\n  ~DictItem\(\) \{\r?\n.*?^  \}\r?\n//ms' "$H"
echo "  --- the struct as it now reads ---"
sed -n '/^struct DictItem/,/^};/p' "$H" | sed 's/^/    /'
c "trivial DictItem, -O2" -O2 -g
cp -f $D/h.orig "$H"

echo
echo "=== and with the default member initialiser dropped instead ==="
sed -i 's/mDictItemArray\[65536\] = {}/mDictItemArray[65536]/' "$H"
c "no '= {}', -O2" -O2 -g
cp -f $D/h.orig "$H"

echo
echo "=== header restored ==="
md5sum $D/h.orig "$H" | sed 's/^/  /'
