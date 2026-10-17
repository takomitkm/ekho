#!/bin/bash
# ASCII only. ekho_dict.cpp fails with NO diagnostic at all, which is not what a
# compiler error looks like. Show the command make runs, then run it by hand.
. /c/Users/YiZhidi/x-common.sh
set -u
cd "$XROOT/src/ekho-linux" || exit 1

echo "=== the command itself (V=1) ==="
rm -f src/libekho_a-ekho_dict.o
make V=1 src/libekho_a-ekho_dict.o 2>&1 | tail -12 | sed 's/^/  /'

echo
echo "=== run the same compiler invocation directly ==="
CMD=$(make V=1 -n src/libekho_a-ekho_dict.o 2>/dev/null | grep -m1 'ekho_dict')
echo "  cmd length: ${#CMD} chars"
echo "$CMD" | cut -c1-400 | sed 's/^/    /'
echo "  ..."
echo
eval "$CMD"; rc=$?
echo "  direct rc=$rc"
ls -l src/libekho_a-ekho_dict.o 2>/dev/null | sed 's/^/  /' || echo "  object still absent"

echo
echo "=== how big is ekho_dict.cpp, and does it compile standalone? ==="
wc -l src/ekho_dict.cpp | sed 's/^/  /'
echo
echo "=== try again with the C++17 flag alone, no other CPPFLAGS ==="
"$CXX" -std=gnu++17 -O0 -fsyntax-only -DHAVE_CONFIG_H -I. -I./src \
    -I/d/yzd/ekho-x/kit-sndfile/include -I/d/yzd/ekho-x/kit-espeak/include \
    -I/d/yzd/MSYS2/mingw64/include/utf8cpp \
    src/ekho_dict.cpp 2>&1 | head -20 | sed 's/^/  /'
echo "  syntax-only rc=$?"
