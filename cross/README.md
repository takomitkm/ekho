# cross -- how the aarch64 kits are built

The scripts here produce the five kits under the school machine's
`/d/yzd/wcis/crosswin/ekho`: one directory per library, each self-contained,
checksummed and relocatable. They are kept with the fork because the kits are
part of what the fork ships, and because these scripts are the procedure, not
just notes.

They run on a Windows host -- MSYS2 bash for the make/autotools plumbing, the
Arm GNU Toolchain 10.3-2021.07 (mingw-w64-i686 hosted) for the compiler -- and
drive the school machine over ssh (`school-guest-lan`), where the build actually
happens. Every path is specific to that machine pair (`XROOT`, `TC`, the MSYS2
prefix, `C:/Users/YiZhidi`); `x-common.sh` is where they are defined.

## What a kit is

Headers, a static archive, a pkg-config file whose prefix is
`${pcfiledir}/../..`, a checksummed `MANIFEST.txt`, and a machine-readable
`kit.env`. A consumer points `PKG_CONFIG_LIBDIR` at the kit and gets working
flags without touching the build tree. `verify-kits.sh <kitdir>...` re-checks
every file against the kit's own manifest, so a kit stays verifiable anywhere
after it is moved.

    kit-sonic          libsonic, from this tree
    kit-sndfile        libsndfile 1.0.28
    kit-espeak         espeak-ng 1.50+dfsg (static)
    kit-glibc-compat   the __xstat shim that lets the glibc 2.33 toolchain
                       produce binaries for the board's glibc 2.31
    kit-ekho           ekho, ekho-stream, ekho-server + the data they need

## Order

    x-get.sh                fetch the source tarballs (USTC ubuntu-ports pool;
                            github is unreachable from both machines)
    x-unpack.sh             extract them under $XROOT/src
    x-kit-glibc-compat.sh   must exist before anything that includes the shim
    x-kit-sonic.sh
    x-kit-sndfile.sh
    x-kit-espeak.sh
    x-kit-ekho.sh           build this fork against the kits, finish kit-ekho
    x-kit-all.sh            the whole sequence
    x-rebuild-all.sh        glibc-compat -> sndfile -> espeak -> ekho
    verify-kits.sh          size + md5 for each kit, from its own manifest
    x-pack-kit.sh           verify, then tar a kit for transport (no rebuild)

`guest-bash.ps1` uploads one of these to the guest and runs it there with the
guest's MSYS2 bash -- the default remote shell is cmd, which cannot run a .sh
file. Uploading also keeps the guest's own copy of the scaffolding current.

## Licence

These scripts are part of the fork and carry its licence. The kits they produce
bundle third-party code under its own terms: see `../third_party/README.md`, and
the `share/doc/` directory inside kit-ekho, which the build fills with the
notices that have to travel with the binaries.

## diag/

Historical diagnostic scripts from the cross-build session (x-diag1..24,
x-probe1..5, x-recon-*), kept as the record of what was measured and why. The
current build path does not call them.
