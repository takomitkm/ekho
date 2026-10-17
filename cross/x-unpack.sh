#!/bin/bash
# ASCII only. Unpack the forked ekho tree into the cross-build workspace.
set -u
# Same default as x-common.sh; override with XROOT=... if the workspace moved.
W=${XROOT:-/d/yzd/wcis/crosswin/ekho}
mkdir -p "$W/src" "$W/build" "$W/logs"
rm -rf "$W/src/ekho-linux"
mkdir -p "$W/src/ekho-linux"
tar -xzf /c/Users/YiZhidi/ekho-linux-0d62176.tar.gz -C "$W/src/ekho-linux" || exit 1
echo "=== extracted ==="
cd "$W/src/ekho-linux" || exit 1
echo "  files: $(find . -type f | wc -l)"
echo "  size:  $(du -sh . | cut -f1)"
echo "  third_party/sonic: $(ls third_party/sonic 2>/dev/null | tr '\n' ' ')"
echo "  third_party/alsa:  $(ls third_party/alsa/include/alsa 2>/dev/null | wc -l) headers"
echo "  ekho-data files:   $(ls ekho-data 2>/dev/null | wc -l)  (no .voice: that is by design)"
echo
echo "=== the tree is the committed one: HEAD of the dev machine repo ==="
echo "  src/ files: $(ls src | wc -l)"
ls -1 src | grep -E 'alsa_backend|streaming|text_normalize|http_server|ekho_server_main|ekho_stream_main' | sed 's/^/    /'
