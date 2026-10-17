#!/bin/bash
# ASCII only. espeak-ng's configure wants its autotools helpers, and the DFSG
# tarball may not carry them. Find out which are missing.
. /c/Users/YiZhidi/x-common.sh
set -u
S=$XROOT/src/espeak-ng-1.50+dfsg
cd "$S" || die "no $S"

echo "=== every file at the top level (full list) ==="
ls -1a | sed 's/^/  /'

echo
echo "=== the autotools helpers configure looks for ==="
for f in install-sh install.sh missing config.guess config.sub ltmain.sh compile depcomp ylwrap \
         build-aux/install-sh .autotools/install-sh m4/install-sh; do
    printf '  %-26s %s\n' "$f" "$(test -e "$f" && echo present || echo '-')"
done

echo
echo "=== AC_CONFIG_AUX_DIR / AC_CONFIG_MACRO_DIR in configure.ac ==="
grep -nE 'AC_CONFIG_AUX_DIR|AC_CONFIG_MACRO_DIR|LT_INIT|AM_INIT_AUTOMAKE|AC_PROG_LIBTOOL|AC_CONFIG_HEADERS' configure.ac | sed 's/^/  /'

echo
echo "=== the helper directory, whatever it is called ==="
for d in .autotools build-aux aux; do
    [ -d "$d" ] && { echo "  $d/"; ls -1 "$d" | sed 's/^/    /'; }
done

echo
echo "=== what MSYS2 can supply ==="
for f in install-sh missing config.guess config.sub ltmain.sh compile depcomp; do
    printf '  %-14s %s\n' "$f" "$(ls /usr/share/automake-1.11/$f /usr/share/libtool/build-aux/$f 2>/dev/null | head -1)"
done
echo "  automake: $(automake --version | head -1)"
echo "  autoconf: $(autoconf --version | head -1)"
echo "  libtool:  $(libtool --version 2>/dev/null | head -1)"
echo "  libtoolize: $(command -v libtoolize)"

echo
echo "=== does autogen.sh just call autoreconf? ==="
head -30 autogen.sh | sed 's/^/  /'
