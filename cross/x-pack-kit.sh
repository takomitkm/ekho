#!/bin/bash
# ASCII only. Pack the kit that is already in the workspace -- no rebuild.
#
# x-publish-kit.sh rebuilds first; this is the other half on its own, for when
# the kit was just rebuilt (or verified) and you only want a fresh tar to copy
# back to the dev machine.
#
#   bash x-pack-kit.sh [kitname]        default: kit-ekho
#
# The tar's top-level entry is <kitname>/, so extracting it anywhere gives you
# one directory. Verify what you get with verify-kits.sh.
. /c/Users/YiZhidi/x-common.sh
set -u

KITNAME=${1:-kit-ekho}
KIT=$XROOT/$KITNAME
TAR=/c/Users/YiZhidi/$KITNAME.tar

[ -d "$KIT" ] || { echo "no $KIT"; exit 1; }
[ -f "$KIT/MANIFEST.txt" ] || { echo "$KIT has no MANIFEST.txt (did the build finish?)"; exit 1; }

echo "=== verify before packing ==="
bash /c/Users/YiZhidi/verify-kits.sh "$KIT" || { echo "verify FAILED -- not packing"; exit 1; }

echo
echo "=== pack ==="
# tar from the parent of the kit so the archive root is <kitname>/, and let the
# transform drop the workspace layer (the workspace dir is not the kit's name).
cd "$XROOT/.." || exit 1
rm -f "$TAR"
tar cf "$TAR" "$(basename "$XROOT")/$KITNAME" --transform "s,^$(basename "$XROOT")/,," || { echo "tar FAILED"; exit 1; }
ls -la "$TAR" | sed 's/^/  /'
echo "  files in kit : $(find "$KIT" -type f | wc -l)"
echo "  kit du       : $(du -sh "$KIT" | cut -f1)"
echo "  manifest lines: $(wc -l < "$KIT/MANIFEST.txt")"
echo "  tar entries  : $(tar tf "$TAR" | wc -l)"
echo "  first entries:"
tar tf "$TAR" | head -3 | sed 's/^/    /'
