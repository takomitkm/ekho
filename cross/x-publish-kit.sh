#!/bin/bash
# ASCII only. Pack the built kit-ekho for the dev machine to pick up, and show
# what is inside it.
#
# This only packs -- run x-kit-ekho.sh first. Packing without rebuilding is the
# point: the tar then holds exactly the tree x-kit-ekho.sh just gated and
# verify-kits.sh checks again below, not a second build that nobody looked at.
. /c/Users/YiZhidi/x-common.sh
set -u

KIT=$XROOT/kit-ekho
[ -d "$KIT" ] || { echo "no $KIT -- run x-kit-ekho.sh first"; exit 1; }

echo "############ 1. what is about to be packed"
echo "  source: $(sed -n 's/^KIT_SOURCE=//p' "$KIT/kit.env")"
echo "  files:  $(find "$KIT" -type f | wc -l)"
echo "  du:     $(du -sh "$KIT" | cut -f1)"
echo "  manifest lines: $(wc -l < "$KIT/MANIFEST.txt")"

echo
echo "############ 2. self-check against the kit's own manifest"
bash /c/Users/YiZhidi/verify-kits.sh "$KIT" || { echo "verify FAILED"; exit 1; }

echo
echo "############ 3. licences in the kit"
ls -la "$KIT/share/doc" | sed 's/^/  /'
echo "  --- SOURCE-AND-LICENCES.txt ---"
sed 's/^/  | /' "$KIT/share/doc/SOURCE-AND-LICENCES.txt"

echo
echo "############ 4. pack for download"
cd "$XROOT" || exit 1
tar cf /c/Users/YiZhidi/kit-ekho.tar kit-ekho || { echo "tar FAILED"; exit 1; }
ls -la /c/Users/YiZhidi/kit-ekho.tar | sed 's/^/  /'
echo "PACK-OK"
