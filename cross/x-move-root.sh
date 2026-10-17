#!/bin/bash
# ASCII only. Move the ekho cross-build workspace into the shared cross area
# (/d/yzd/wcis/crosswin, the WCIS convention) and prove nothing broke.
. /c/Users/YiZhidi/x-common.sh
set -u

OLD=/d/yzd/ekho-x
NEW=/d/yzd/wcis/crosswin/ekho

echo "=== 1. move $OLD -> $NEW (same volume: a rename, not a copy) ==="
mkdir -p /d/yzd/wcis/crosswin
if [ -d "$NEW" ]; then
    echo "  target already exists, leaving it alone"
else
    [ -d "$OLD" ] || { echo "  no $OLD"; exit 1; }
    mv "$OLD" "$NEW" || { echo "  mv FAILED"; exit 1; }
    echo "  moved"
fi
du -sh "$NEW" 2>/dev/null | sed 's/^/  /'
ls -1 "$NEW" | sed 's/^/    /'

echo
echo "=== 2. every kit verified against its own manifest, in the new place ==="
bash /c/Users/YiZhidi/verify-kits.sh "$NEW"/kit-*
rc=$?

echo
echo "=== 3. the shipped binaries are still aarch64 ==="
for b in ekho ekho-stream ekho-server; do
    printf '  %-12s %s\n' "$b" \
      "$("$READELF" -h "$NEW/kit-ekho/bin/$b" | grep -E 'Class:|Machine:|Type:' | tr -s ' ' | tr '\n' ' ')"
done

echo
echo "=== 4. the link-time .pc files still point at their own kit, not at \$XROOT ==="
grep -h '^prefix' "$NEW"/kit-*/lib/pkgconfig/*.pc 2>/dev/null | sort -u | sed 's/^/  /'

echo
echo "=== 5. no script may still name the old root ==="
grep -l '/d/yzd/ekho-x' /c/Users/YiZhidi/*.sh 2>/dev/null | sed 's/^/  STALE: /' || true

echo
echo "=== 6. disk ==="
df -h /d | tail -1 | sed 's/^/  /'

exit $rc
