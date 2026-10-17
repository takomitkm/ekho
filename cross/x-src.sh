#!/bin/bash
# ASCII only. Does the USTC pool carry the upstream source tarballs (.orig)?
M=http://mirrors.ustc.edu.cn/ubuntu-ports
for P in pool/main/libs/libsndfile pool/universe/e/espeak-ng; do
  echo "=== $M/$P ==="
  curl -sS --max-time 40 "$M/$P/" | grep -oE 'href="[^"]+"' | sed 's/href="//;s/"//' \
    | grep -E '\.(orig\.tar|dsc|debian\.tar)' | sed 's/^/  /'
done

echo
echo "=== espeak-ng dev/data binary packages (for the fallback donor route) ==="
curl -sS --max-time 40 "$M/pool/universe/e/espeak-ng/" | grep -oE 'href="[^"]+"' | sed 's/href="//;s/"//' \
  | grep -E 'libespeak-ng-(dev|data|1).*arm64\.deb' | sed 's/^/  /'

echo
echo "=== focal (20.04) versions of each, straight from the index ==="
for C in main universe; do
  curl -sS --max-time 60 "$M/dists/focal/$C/binary-arm64/Packages.gz" \
    | gzip -dc 2>/dev/null | awk '
        /^Package: / {p=$2}
        /^Version: / {v=$2}
        /^Filename: / {f=$2; if (p ~ /^(libsndfile1-dev|libsndfile1|libespeak-ng-dev|libespeak-ng1|espeak-ng|espeak-ng-data|libsonic-dev|libsonic0)$/) printf "  %-18s %-22s %s\n", p, v, f}
      '
done
