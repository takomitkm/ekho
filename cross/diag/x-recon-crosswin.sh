#!/bin/bash
# ASCII only. What is in wcis/crosswin?
echo "=== /d/yzd/wcis/crosswin ==="
ls -la /d/yzd/wcis/crosswin/ 2>&1 | head -40
echo
echo "=== one level deeper ==="
for d in /d/yzd/wcis/crosswin/*/; do
  [ -d "$d" ] || continue
  echo "--- $d"
  ls -la "$d" 2>&1 | head -12
done
echo
echo "=== du (depth 1) ==="
du -sh /d/yzd/wcis/crosswin/* 2>/dev/null | head -20
echo
echo "=== wcis/fdw ==="
ls -la /d/yzd/wcis/fdw/ 2>&1 | head -15
