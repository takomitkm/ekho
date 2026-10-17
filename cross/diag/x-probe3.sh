#!/bin/bash
# ASCII only. espeak-ng is the awkward one: its build normally ends by RUNNING
# the espeak-ng it just built, to compile the phoneme tables into
# espeak-ng-data. That cannot work when the binary is aarch64 and the host is
# Windows. Find out exactly how much of the build needs that.
. /c/Users/YiZhidi/x-common.sh
set -u
S=$XROOT/src/espeak-ng-1.50+dfsg
cd "$S" || die "no $S"

echo "=== top level ==="
ls -1 | head -30 | sed 's/^/  /'

echo
echo "=== build system present? ==="
for f in configure configure.ac autogen.sh Makefile.am src/Makefile.am data/Makefile.am; do
    printf '  %-22s %s\n' "$f" "$(test -e "$f" && echo present || echo MISSING)"
done

echo
echo "=== the library target ==="
grep -n -A6 'libespeak' src/Makefile.am 2>/dev/null | head -40 | sed 's/^/  /'

echo
echo "=== anything that runs the built binary? ==="
grep -rn 'compile=phonemes\|compile=intonations\|compile-phonemes\|--compile' \
     --include='Makefile.am' --include='*.ac' --include='*.sh' --include='*.mk' \
     --include='Makefile.in' . 2>/dev/null | head -15 | sed 's/^/  /'

echo
echo "=== data/ (the phoneme sources) ==="
ls -1 data 2>/dev/null | head -20 | sed 's/^/  /'

echo
echo "=== espeak-ng-data/ (generated, if shipped) ==="
if [ -d espeak-ng-data ]; then
    ls -1 espeak-ng-data | head -20 | sed 's/^/  /'
    echo "  total: $(find espeak-ng-data -type f | wc -l) files, $(du -sh espeak-ng-data | cut -f1)"
else
    echo "  absent -- the generated tables are not in the tarball"
fi

echo
echo "=== configure.ac: the switches that matter ==="
grep -nE 'AC_ARG_(WITH|ENABLE)\(\[?([a-z0-9-]+)' configure.ac 2>/dev/null | sed 's/^/  /'

echo
echo "=== focal espeak-ng-data deb: what is inside ==="
D=/tmp/edbg; rm -rf $D; mkdir -p $D; cd $D || exit 1
bsdtar -xf "$XROOT/dl/espeak-ng-data_1.50+dfsg-6_arm64.deb" 2>/dev/null
INNER=$(ls data.tar.* 2>/dev/null | head -1)
echo "  inner archive: ${INNER:-MISSING}"
if [ -n "$INNER" ]; then
    echo "  entries: $(bsdtar -tf "$INNER" | wc -l)"
    bsdtar -tf "$INNER" | grep -vE '/$' | head -12 | sed 's/^/    /'
    echo "    ..."
    bsdtar -tf "$INNER" | grep -E 'phondata|phontab|phonindex|intonations|_dict$' | head -10 | sed 's/^/    /'
fi

echo
echo "=== and the espeak-ng-dev deb, in case the library build fights back ==="
cd $D || exit 1
bsdtar -xf "$XROOT/dl/libsonic-dev_0.2.0-8_arm64.deb" 2>/dev/null && echo "  (sonic dev deb opened ok)"
ls -1 /tmp/edbg | sed 's/^/    /'
