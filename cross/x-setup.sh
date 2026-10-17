#!/bin/bash
# ASCII only. Unpack the upstream sources, tolerating the symlinks the
# espeak-ng tarball carries and Windows will not always create.
. /c/Users/YiZhidi/x-common.sh

mkdir -p "$XROOT/dl" "$XROOT/src" "$XROOT/build" "$XROOT/logs"
cd "$XROOT/src" || die "no $XROOT/src"

unpack() {  # unpack <tarball-name> <expected-dir>
    local tb=$XROOT/dl/$1 exp=$2
    if [ -d "$exp" ]; then echo "  have      $exp"; return 0; fi
    [ -f "$tb" ] || { echo "  MISSING   $tb"; return 1; }
    # A symlink inside the tarball (espeak-ng ships README.md -> README) makes
    # Windows tar exit non-zero part-way through. That is cosmetic here: the
    # build never reads those files, so the extraction result is judged by
    # whether the directory arrived, not by tar's exit status.
    tar -xzf "$tb" >/dev/null 2>&1
    if [ -d "$exp" ]; then echo "  unpacked  $exp"; else echo "  FAILED    $exp"; return 1; fi
}

echo "=== sources ==="
unpack libsndfile_1.0.28.orig.tar.gz        libsndfile-1.0.28
unpack "espeak-ng_1.50+dfsg.orig.tar.gz"    "espeak-ng-1.50+dfsg"

echo
echo "=== toolchain sanity ==="
"$CC" --version | head -1
CS=$(ls /usr/share/automake-*/config.sub 2>/dev/null | head -1)
echo "  config.sub ($CS)"
echo "    aarch64-none-linux-gnu -> $(bash "$CS" aarch64-none-linux-gnu 2>/dev/null)"
echo "    aarch64-linux-gnu      -> $(bash "$CS" aarch64-linux-gnu 2>/dev/null)"
echo "  msys build triple: $(bash "$(dirname "$CS")/config.guess" 2>/dev/null)"

echo
echo "=== libsndfile 1.0.28: configure options that matter ==="
cd "$XROOT/src/libsndfile-1.0.28" || exit 1
./configure --help 2>/dev/null | grep -E '^  --(enable|disable|with|without)' | sed 's/^/  /'
