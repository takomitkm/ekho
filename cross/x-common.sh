#!/bin/bash
# ASCII only. Shared settings for the ekho aarch64 cross-build.
#
# Cross builds on this machine live under /d/yzd/wcis/crosswin (the convention
# the WCIS cross-compile pages settled on; ref/board-libs.txt is the board
# inventory snapshot the gates read). One subdirectory per project.
#
# Layout (everything under one root, so the whole thing is one moveable tree):
#   $XROOT/src/          upstream sources, extracted
#   $XROOT/dl/           the tarballs/debs they came from
#   $XROOT/build/        scratch build dirs
#   $XROOT/kit-sonic/    } one directory per library -- headers, a static
#   $XROOT/kit-sndfile/  } archive, its .pc file, and a MANIFEST describing
#   $XROOT/kit-espeak/   } exactly what is in it and where it came from
#   $XROOT/kit-ekho/     the linked binaries plus the runtime data they need
#   $XROOT/logs/
#
# The kits do not reference $XROOT: a .pc file uses ${pcfiledir} and a consumer
# points PKG_CONFIG_LIBDIR at the kit, so a kit can be lifted out and dropped
# anywhere else. That is the "keep the libraries separate, easy to move" part.

: "${XROOT:=/d/yzd/wcis/crosswin/ekho}"

# The Arm GNU toolchain, mingw-w64-i686-hosted, targeting aarch64 linux gnu.
# Its own default sysroot is $TC/aarch64-none-linux-gnu/libc, which on this
# toolchain keeps the libs under lib64/ and usr/lib64/ -- gcc knows, so no
# --sysroot is needed and passing one would only break it.
: "${TC:=/d/yzd/tools/cross/gcc-arm-10.3-2021.07-mingw-w64-i686-aarch64-none-linux-gnu}"
XBIN="$TC/bin"
CC="$XBIN/aarch64-none-linux-gnu-gcc.exe"
CXX="$XBIN/aarch64-none-linux-gnu-g++.exe"
AR="$XBIN/aarch64-none-linux-gnu-ar.exe"
RANLIB="$XBIN/aarch64-none-linux-gnu-ranlib.exe"
STRIP="$XBIN/aarch64-none-linux-gnu-strip.exe"
READELF="$XBIN/aarch64-none-linux-gnu-readelf.exe"
OBJDUMP="$XBIN/aarch64-none-linux-gnu-objdump.exe"
NM="$XBIN/aarch64-none-linux-gnu-nm.exe"
CONFIG_SUB="$XBIN/../share/autoconf/config.sub"

# Host side: the toolchain ships no make, so the host tools come from MSYS2.
MINGW=/d/yzd/MSYS2/mingw64
export PATH="$XBIN:$MINGW/bin:/usr/bin:$PATH"

# The board's userspace ceilings, from the factory rootfs:
#   libc6 2.31-0ubuntu9.9, libstdc++6 10.3.0-1ubuntu1~20.04.
# Anything above these will not load, however well it links.
BOARD_GLIBC_MAX=2.31
BOARD_GLIBCXX_MAX=3.4.28
BOARD_CXXABI_MAX=1.3.11

die() { echo "FATAL: $*" >&2; exit 1; }

# ---------------------------------------------------------------- kit helpers

# Write the two files that make a kit self-describing: a checksummed inventory
# and a machine-readable summary. Both are regenerated, never hand-edited.
#
#   kit_finish <kitdir> <name> <version> <where it came from> <what it is>
kit_finish() {
    local dir=$1 name=$2 version=$3 src=$4 what=$5
    [ -d "$dir" ] || die "kit_finish: $dir does not exist"
    {
        echo "# $name $version"
        echo "# built for : aarch64-none-linux-gnu"
        echo "# built with: $(basename "$CC") (Arm GNU Toolchain 10.3-2021.07)"
        echo "# built on  : $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        echo "# source    : $src"
        echo "# contents  : $what"
        echo "#"
        echo "# Verify a kit anywhere with:  bash verify-kits.sh <kitdir> ..."
        echo "#"
        echo "# bytes      md5                              file"
    } > "$dir/MANIFEST.txt"
    # %P is the path relative to the kit root, which is what makes the manifest
    # survive being moved.
    ( cd "$dir" && find . -type f ! -name MANIFEST.txt ! -name kit.env -printf '%s %P\n' ) \
        | sort -k2 | while read -r size rel; do
            printf '%10s %s %s\n' "$size" "$(md5sum "$dir/$rel" | cut -d' ' -f1)" "$rel"
        done >> "$dir/MANIFEST.txt"

    cat > "$dir/kit.env" <<EOF
# Machine-readable summary of this kit. Paths in here are relative to the kit
# root: resolve them against wherever the kit ended up, not against $XROOT.
KIT_NAME=$name
KIT_VERSION=$version
KIT_TARGET=aarch64-none-linux-gnu
KIT_SOURCE=$src
KIT_WHAT=$what
KIT_INCLUDE_DIR=include
KIT_LIB_DIR=lib
KIT_SHARE_DIR=share
KIT_PC_DIR=lib/pkgconfig
EOF
    echo "  kit written: $dir ($(find "$dir" -type f | wc -l) files, $(du -sh "$dir" | cut -f1))"
}

kit_reset() { rm -rf "$1"; mkdir -p "$1"; }
