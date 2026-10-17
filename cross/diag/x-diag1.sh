#!/bin/bash
# ASCII only. Show what the espeak-ng link actually failed on.
L=/d/yzd/ekho-x/logs/espeak-make.log
echo "=== last 60 lines of the make log ==="
tail -60 "$L" | sed 's/^/  /'
echo
echo "=== every line with 'error' plus 4 lines of context before each ==="
grep -nE 'error|undefined reference|cannot find' "$L" | head -20 | sed 's/^/  /'
echo
echo "=== the link command lines used ==="
grep -nE '^libtool:| -o src/(espeak-ng|speak-ng)|LD +' "$L" | head -12 | sed 's/^/  /'
