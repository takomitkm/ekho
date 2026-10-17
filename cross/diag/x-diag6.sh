#!/bin/bash
# ASCII only. ekho_dict.cpp exits 1 with nothing on stdout or stderr. A compiler
# error prints something; a compiler that dies does not. Find out which phase.
. /c/Users/YiZhidi/x-common.sh
set -u
cd "$XROOT/src/ekho-linux" || exit 1

CMD=$(make V=1 -n src/libekho_a-ekho_dict.o 2>/dev/null | grep -m1 'g++')
D=/tmp/xdict; rm -rf $D; mkdir -p $D

echo "=== 1. exact command, stdout and stderr captured separately ==="
eval "$CMD" > $D/out.txt 2> $D/err.txt
rc=$?
echo "  rc=$rc   stdout=$(stat -c%s $D/out.txt) bytes   stderr=$(stat -c%s $D/err.txt) bytes"
echo "  --- stdout ---"; head -20 $D/out.txt | sed 's/^/    /'
echo "  --- stderr ---"; head -20 $D/err.txt | sed 's/^/    /'

echo
echo "=== 2. which phase dies: save the preprocessed source and the assembly ==="
cd $D || exit 1
eval "$CMD" --save-temps=obj -o $D/d.o > $D/out2.txt 2> $D/err2.txt
echo "  rc=$?   stderr $(stat -c%s $D/err2.txt) bytes"
ls -l $D 2>/dev/null | sed 's/^/  /'

echo
echo "=== 3. -O0 instead of -O2 (optimiser?) ==="
cd "$XROOT/src/ekho-linux" || exit 1
CMD0=$(echo "$CMD" | sed 's/ -O2 / -O0 /')
eval "$CMD0" -o $D/o0.o > $D/out3.txt 2> $D/err3.txt
echo "  rc=$?   stderr: $(head -3 $D/err3.txt | tr '\n' ' ')"

echo
echo "=== 4. without the dependency-file flags (-MD -MP -MF ...) ==="
CMDN=$(echo "$CMD" | sed 's/ -MD -MP -MF [^ ]*//')
eval "$CMDN" -o $D/on.o > $D/out4.txt 2> $D/err4.txt
echo "  rc=$?   stderr: $(head -3 $D/err4.txt | tr '\n' ' ')"

echo
echo "=== 5. does the .deps directory exist at all? ==="
ls -ld src/.deps 2>/dev/null | sed 's/^/  /' || echo "  src/.deps is MISSING"

echo
echo "=== 6. how many #includes, and which large headers ==="
grep -c '^#include' src/ekho_dict.cpp | sed 's/^/  includes: /'
grep -n '^#include' src/ekho_dict.cpp | head -30 | sed 's/^/  /'
