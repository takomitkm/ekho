#!/bin/bash
# ASCII only. What did the ekho cross-build stop on this time?
L=/d/yzd/ekho-x/logs/ekho-make.log
echo "=== last 45 lines ==="
tail -45 "$L" | sed 's/^/  /'
echo
echo "=== lines mentioning error/Error/undefined ==="
grep -nE 'error|Error|undefined reference|cannot find|No such file' "$L" | head -25 | sed 's/^/  /'
echo
echo "=== how many objects compiled ok ==="
echo "  CXX lines: $(grep -c 'CXX' "$L")"
echo "  CC  lines: $(grep -c '  CC ' "$L")"
echo "  CCLD lines: $(grep -c 'CCLD' "$L")"
