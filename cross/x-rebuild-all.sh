#!/bin/bash
# ASCII only. Rebuild in dependency order, because the glibc shim has to reach the
# static libraries too: libsndfile.a carries `U fstat` and libespeak-ng.a carries
# `U stat`, and those become the executable's own imports when it links them in.
#
#   kit-glibc-compat  (the shim itself)
#        |
#        +-- kit-sndfile --+
#        +-- kit-espeak  --+-- kit-ekho
#        +-- kit-sonic   --+
set -u
for s in glibc-compat sndfile espeak ekho; do
    echo
    echo "################################################################"
    echo "############  kit-$s"
    echo "################################################################"
    bash "/c/Users/YiZhidi/x-kit-$s.sh" || { echo; echo "### kit-$s FAILED, stopping"; exit 1; }
done
echo
echo "################################################################"
echo "############  all three rebuilt"
echo "################################################################"
