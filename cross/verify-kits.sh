#!/bin/bash
# ASCII only. Verify one or more kits against their own MANIFEST.txt.
#
#   bash verify-kits.sh <kitdir> [<kitdir> ...]
#
# Every kit carries a checksummed inventory whose paths are relative to the kit
# root, so this works anywhere: copy a kit to the board, run this there, and it
# tells you whether the kit survived the trip. Needs only bash, find, stat,
# md5sum and awk.
set -u
[ $# -ge 1 ] || { echo "usage: $0 <kitdir> [...]" >&2; exit 2; }

bad=0; kits=0; files=0
for kit in "$@"; do
    if [ ! -d "$kit" ]; then echo "MISSING      $kit"; bad=$((bad+1)); continue; fi
    if [ ! -f "$kit/MANIFEST.txt" ]; then echo "NO MANIFEST  $kit"; bad=$((bad+1)); continue; fi
    kits=$((kits+1))
    name=$(awk '/^KIT_NAME=/{sub(/^KIT_NAME=/,"");print;exit}' "$kit/kit.env" 2>/dev/null)
    what=$(awk '/^KIT_WHAT=/{sub(/^KIT_WHAT=/,"");print;exit}' "$kit/kit.env" 2>/dev/null)
    echo "=== $(basename "$kit")${name:+  [$name]}"
    [ -n "$what" ] && echo "    $what"
    n=0; m=0
    while read -r size md5 rel; do
        case "$size" in ''|\#*) continue ;; esac
        f="$kit/$rel"
        n=$((n+1))
        if [ ! -f "$f" ]; then echo "    MISSING  $rel"; m=$((m+1)); continue; fi
        got=$(stat -c%s "$f")
        if [ "$got" != "$size" ]; then echo "    SIZE     $rel ($got != $size)"; m=$((m+1)); fi
        g=$(md5sum "$f" | cut -d' ' -f1)
        if [ "$g" != "$md5" ]; then echo "    MD5      $rel"; m=$((m+1)); fi
    done < "$kit/MANIFEST.txt"
    files=$((files+n))
    if [ "$m" = 0 ]; then
        echo "    ok  $n files"
    else
        echo "    FAILED  $m of $n"
        bad=$((bad+1))
    fi
done
echo
echo "kits: $kits   files: $files   bad: $bad"
[ "$bad" = 0 ] || exit 1
