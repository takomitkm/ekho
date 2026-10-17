#!/bin/bash
# ASCII only. Walk the compiler through its phases on ekho_dict.cpp so the silent
# failure has to show itself somewhere. Argument array, not string surgery, so
# nothing here can be blamed on quoting.
. /c/Users/YiZhidi/x-common.sh
set -u
cd "$XROOT/src/ekho-linux" || exit 1

D=/tmp/xdict; rm -rf $D; mkdir -p $D
echo "=== what --save-temps actually said last time (it was my extra -o, not gcc) ==="
head -5 $D/err2.txt 2>/dev/null | sed 's/^/  /'

INC=(-std=gnu++17 -g -pthread -DHAVE_CONFIG_H -I. -I./src
     -I"$XROOT/kit-sndfile/include" -I"$XROOT/kit-espeak/include"
     -I./third_party/sonic -I./third_party/alsa/include
     -I/d/yzd/MSYS2/mingw64/include/utf8cpp
     -DEKHO_DATA_PATH='"/d/yzd/ekho-x/kit-ekho/share/ekho-data"')

run() {
    local desc=$1; shift
    "$@" > $D/o 2> $D/e; local rc=$?
    printf '  %-26s rc=%-3s out=%-6s err=%-6s\n' "$desc" "$rc" "$(stat -c%s $D/o)" "$(stat -c%s $D/e)"
    head -6 $D/e | sed 's/^/        /'
}

echo
echo "=== phase by phase ==="
run "preprocess only (-E)"     "$CXX" "${INC[@]}" -E src/ekho_dict.cpp -o $D/x.ii
run "syntax only"              "$CXX" "${INC[@]}" -fsyntax-only src/ekho_dict.cpp
run "assembly, -O0"            "$CXX" "${INC[@]}" -O0 -S src/ekho_dict.cpp -o $D/x0.s
run "assembly, -O2"            "$CXX" "${INC[@]}" -O2 -S src/ekho_dict.cpp -o $D/x2.s
run "compile, -O0"             "$CXX" "${INC[@]}" -O0 -c src/ekho_dict.cpp -o $D/x0.o
run "compile, -O2"             "$CXX" "${INC[@]}" -O2 -c src/ekho_dict.cpp -o $D/x2.o
run "compile, -O2, no -g"      "$CXX" "${INC[@]}" -O2 -c src/ekho_dict.cpp -o $D/x2ng.o
run "compile, -g0 -O2"         "$CXX" "${INC[@]}" -g0 -O2 -c src/ekho_dict.cpp -o $D/x2g0.o

echo
echo "=== outputs that did appear ==="
ls -l $D | sed 's/^/  /'

echo
echo "=== preprocessed size (a runaway include shows up here) ==="
[ -f $D/x.ii ] && wc -lc $D/x.ii | sed 's/^/  /'

echo
echo "=== the same thing through cmd.exe, to catch a Windows-level loader error ==="
cd "$XROOT/src/ekho-linux" || exit 1
cygpath -w "$CXX" > $D/cxxpath.txt
CXXW=$(cat $D/cxxpath.txt)
cmd //c "\"$CXXW\" -std=gnu++17 -O2 -g -pthread -DHAVE_CONFIG_H -I. -I./src -o NUL -c src/ekho_dict.cpp" > $D/co.txt 2> $D/ce.txt
echo "  cmd rc=$?  out=$(stat -c%s $D/co.txt) err=$(stat -c%s $D/ce.txt)"
head -10 $D/ce.txt | sed 's/^/    /'
