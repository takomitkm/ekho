#!/bin/bash
# ASCII only. What options do the two autotools libraries offer, and can they
# even be configured off this host?
. "$(dirname "$0")/x-common.sh" 2>/dev/null || . /c/Users/YiZhidi/x-common.sh
set -u

echo "=== toolchain sanity ==="
"$CC" --version | head -1
echo "  config.sub knows aarch64-none-linux-gnu? $(/usr/share/automake-*/config.sub aarch64-none-linux-gnu 2>/dev/null | head -1)"
echo "  config.sub knows aarch64-linux-gnu?      $(/usr/share/automake-*/config.sub aarch64-linux-gnu 2>/dev/null | head -1)"
echo "  msys build triple: $(/usr/share/automake-*/config.guess 2>/dev/null)"

echo
echo "=== libsndfile 1.0.28: configure options that matter ==="
cd "$XROOT/src/libsndfile-1.0.28" || exit 1
./configure --help 2>/dev/null | grep -E '^  --(enable|disable|with|without)' | sed 's/^/  /'

echo
echo "=== espeak-ng 1.50: what is in the tarball ==="
cd "$XROOT/src" || exit 1
if [ ! -d espeak-ng-1.50+dfsg ]; then
  tar -xzf "$XROOT/dl/espeak-ng_1.50+dfsg.orig.tar.gz" || exit 1
fi
cd espeak-ng-1.50+dfsg || exit 1
for f in configure configure.ac autogen.sh Makefile.am src/Makefile.am CMakeLists.txt; do
  printf '  %-24s %s\n' "$f" "$(test -e "$f" && echo present || echo MISSING)"
done
echo "  --- src/ ---"
ls -1 src | head -20 | sed 's/^/    /'
echo "  --- does the build try to RUN anything it has built? ---"
grep -rn 'compile-phonemes\|compile=phonemes\|espeak-ng --compile' --include='*.am' --include='*.ac' . 2>/dev/null | head -10 | sed 's/^/    /'
echo "  --- data dir handling ---"
ls -1 data 2>/dev/null | head -10 | sed 's/^/    /'
echo
echo "=== the focal espeak-ng-data deb: what does it hold? ==="
mkdir -p /tmp/edbg
cd /tmp/edbg || exit 1
rm -rf ./*
bsdtar -xf "$XROOT/dl/espeak-ng-data_1.50+dfsg-6_arm64.deb" 2>/dev/null
for part in data.tar.xz data.tar.gz data.tar.zst; do
  [ -f "$part" ] && { echo "  inner: $part"; bsdtar -tf "$part" 2>/dev/null | head -8 | sed 's/^/    /'; \
    echo "    ... total entries: $(bsdtar -tf "$part" 2>/dev/null | wc -l)"; break; }
done
