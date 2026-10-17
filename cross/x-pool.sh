#!/bin/bash
# ASCII only. The box can reach USTC ubuntu-ports but not github, so the three
# libraries have to come from the Ubuntu pool. What is in the pool?
M=http://mirrors.ustc.edu.cn/ubuntu-ports

list() {
  echo "=== $1 ==="
  curl -sS --max-time 40 "$1" | grep -oE 'href="[^"]+"' | sed 's/href="//;s/"//' \
    | grep -vE '^\?|^/ubuntu-ports/pool/.*/../$' | head -40 | sed 's/^/  /'
}

list "$M/pool/main/libs/libsndfile/"
echo
list "$M/pool/universe/e/espeak-ng/"
echo
list "$M/pool/main/s/sonic/"
echo
list "$M/pool/universe/s/sonic/"

echo
echo "=== focal arm64 deb for libsndfile1-dev / libespeak-ng-dev (fallback donors) ==="
for P in "$M/pool/main/libs/libsndfile" "$M/pool/universe/e/espeak-ng"; do
  echo "  --- $P ---"
  curl -sS --max-time 40 "$P/" | grep -oE 'href="[^"]*(dev|data|1_|snap)[^"]*arm64\.deb"' | sed 's/href="//;s/"//' | head -12 | sed 's/^/    /'
done

echo
echo "=== espeak-ng data: is it architecture-independent? ==="
curl -sS --max-time 40 "$M/pool/universe/e/espeak-ng/" | grep -oE 'href="[^"]*data[^"]*"' | sed 's/href="//;s/"//' | head -10 | sed 's/^/  /'
