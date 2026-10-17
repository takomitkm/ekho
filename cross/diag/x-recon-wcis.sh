#!/bin/bash
# ASCII only. What is under /d/yzd/wcis, and what would moving the ekho cross
# work there involve?
echo "=== /d/yzd top level ==="
ls -la /d/yzd/ 2>&1 | head -30
echo
echo "=== /d/yzd/wcis ==="
ls -la /d/yzd/wcis/ 2>&1 | head -30
echo
echo "=== /d/yzd/wcis/cross ==="
ls -la /d/yzd/wcis/cross/ 2>&1 | head -40
echo
echo "=== size under wcis/cross (depth 2) ==="
du -sh /d/yzd/wcis/cross/* 2>/dev/null | head -20
echo
echo "=== current ekho-x ==="
ls -la /d/yzd/ekho-x/ 2>&1 | head -20
du -sh /d/yzd/ekho-x 2>/dev/null
echo
echo "=== disk ==="
df -h /d 2>&1 | tail -2
