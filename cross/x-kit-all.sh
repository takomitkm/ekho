#!/bin/bash
# ASCII only. Run the kit builders in order. Each one is standalone; this is
# just the sequence.
set -u
R=/c/Users/YiZhidi
for s in x-kit-sonic.sh x-kit-sndfile.sh; do
    echo
    echo "############################################################"
    echo "## $s"
    echo "############################################################"
    bash "$R/$s" || { echo "### $s FAILED"; exit 1; }
done
echo
echo "############################################################"
echo "## kits so far"
echo "############################################################"
for k in "${XROOT:-/d/yzd/wcis/crosswin/ekho}"/kit-*; do
    printf '  %-28s %s\n' "$(basename "$k")" "$(du -sh "$k" 2>/dev/null | cut -f1)"
    sed 's/^/      /' "$k/kit.env" 2>/dev/null | grep -E 'VERSION|TARGET|WHAT'
done
