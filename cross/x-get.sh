#!/bin/bash
# ASCII only. Fetch the two upstream libraries that are not vendored in the
# ekho tree, from the USTC ubuntu-ports pool (the box cannot reach github).
#
# Version choice, and it is deliberate: the board's rootfs carries
# libsndfile.so.1.0.28, so the source we build has to be 1.0.28 -- same version,
# same behaviour, and the DT_NEEDED entry the cross-linked ekho will ask for is
# exactly the one the board already answers.
set -u
M=http://mirrors.ustc.edu.cn/ubuntu-ports
# Same default as x-common.sh; override with XROOT=... if the workspace moved.
W=${XROOT:-/d/yzd/wcis/crosswin/ekho}
mkdir -p "$W/dl" "$W/src" "$W/logs"

echo "=== pool listing: espeak-ng (moved to main in focal) ==="
curl -sS --max-time 40 "$M/pool/main/e/espeak-ng/" | grep -oE 'href="[^"]+"' | sed 's/href="//;s/"//' \
  | grep -E 'espeak-ng_1\.50.*(orig\.tar|dsc)|espeak-ng-data_1\.50.*arm64' | sed 's/^/  /'

get() {  # get <url> <localname> <minbytes>
  local url=$1 name=$2 min=$3 out="$W/dl/$2"
  if [ -s "$out" ] && [ "$(stat -c%s "$out")" -ge "$min" ]; then
    echo "  have  $name ($(stat -c%s "$out") B)"; return 0
  fi
  rm -f "$out"
  echo "  fetch $name"
  curl -sSL --max-time 600 -o "$out" "$url" || return 1
  local n=$(stat -c%s "$out" 2>/dev/null || echo 0)
  if [ "$n" -lt "$min" ]; then echo "    too small: $n B (want >= $min)"; rm -f "$out"; return 1; fi
  echo "    got $n B"
}

echo
echo "=== download ==="
get "$M/pool/main/libs/libsndfile/libsndfile_1.0.28.orig.tar.gz" \
    "libsndfile_1.0.28.orig.tar.gz" 500000 || echo "  libsndfile SOURCE FAILED"
get "$M/pool/main/e/espeak-ng/espeak-ng_1.50%2Bdfsg.orig.tar.gz" \
    "espeak-ng_1.50+dfsg.orig.tar.gz" 800000 || echo "  espeak-ng SOURCE FAILED"
get "$M/pool/main/e/espeak-ng/espeak-ng-data_1.50%2Bdfsg-6_arm64.deb" \
    "espeak-ng-data_1.50+dfsg-6_arm64.deb" 2000000 || echo "  espeak-ng-data FAILED"
get "$M/pool/main/libs/libsndfile/libsndfile1_1.0.28-7_arm64.deb" \
    "libsndfile1_1.0.28-7_arm64.deb" 100000 || echo "  libsndfile1 deb FAILED"
get "$M/pool/main/s/sonic/libsonic-dev_0.2.0-8_arm64.deb" \
    "libsonic-dev_0.2.0-8_arm64.deb" 10000 || echo "  libsonic-dev deb FAILED"

echo
echo "=== dl ==="
ls -la "$W/dl" | sed 's/^/  /'

echo
echo "=== extract the sources ==="
cd "$W/src" || exit 1
[ -d libsndfile-1.0.28 ] || tar -xzf "$W/dl/libsndfile_1.0.28.orig.tar.gz" 2>&1 | head -3
for d in libsndfile-1.0.28 espeak-ng-1.50 espeak-ng; do
  [ -d "$d" ] && echo "  present: $d"
done

echo
echo "=== espeak-ng tarball: what does it contain? ==="
tar -tzf "$W/dl/espeak-ng_1.50+dfsg.orig.tar.gz" 2>/dev/null | head -8 | sed 's/^/  /'

echo
echo "=== libsndfile build system ==="
ls "$W/src/libsndfile-1.0.28" 2>/dev/null | head -25 | sed 's/^/  /'
test -x "$W/src/libsndfile-1.0.28/configure" && echo "  configure: YES (autotools)" \
  || { test -f "$W/src/libsndfile-1.0.28/CMakeLists.txt" && echo "  CMakeLists.txt: YES"; }
