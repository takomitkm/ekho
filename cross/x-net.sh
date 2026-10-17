#!/bin/bash
# ASCII only. What can this box actually reach?
echo "=== direct, no proxy ==="
unset HTTP_PROXY HTTPS_PROXY http_proxy https_proxy
for U in \
  http://mirrors.ustc.edu.cn/ubuntu-ports/dists/focal/Release \
  http://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports/dists/focal/Release \
  http://mirrors.aliyun.com/ubuntu-ports/dists/focal/Release \
  http://archive.ubuntu.com/ubuntu-ports/dists/focal/Release \
  http://mirrors.ustc.edu.cn/msys2/mingw/mingw64/ \
  https://github.com \
  https://raw.githubusercontent.com/waywardgeek/sonic/master/sonic.h \
  ; do
  printf '  %-64s ' "$(echo $U | cut -c1-64)"
  code=$(curl -sS -o /tmp/p.out -w '%{http_code}' --max-time 25 "$U" 2>/dev/null)
  echo "http=${code:-ERR} bytes=$(stat -c%s /tmp/p.out 2>/dev/null || echo 0)"
done

echo
echo "=== through the mihomo proxy on 127.0.0.1:7890 ==="
for P in 7890 7891 1080 8080; do
  printf '  port %-5s ' "$P"
  (echo > /dev/tcp/127.0.0.1/$P) >/dev/null 2>&1 && echo "OPEN" || echo "closed"
done
printf '  via proxy: %s\n' "$(curl -sS -o /dev/null -w '%{http_code}' --max-time 20 -x http://127.0.0.1:7890 https://github.com 2>&1 | head -1)"

echo
echo "=== mihomo / proxy leftovers on the box ==="
ls -la /d/yzd/_work/ 2>/dev/null | head -30 | sed 's/^/  /'
for f in /d/yzd/_work/mihomo_run.sh /d/yzd/_work/mihomo_test.sh; do
  echo "  --- $f ---"
  head -40 "$f" 2>/dev/null | sed 's/^/    /'
done

echo
echo "=== is anything listening locally? ==="
netstat -an 2>/dev/null | grep -E 'LISTENING' | head -25 | sed 's/^/  /'

echo
echo "=== pacman cache (sources or packages already downloaded?) ==="
du -sh /d/yzd/MSYS2/var/cache/pacman/pkg 2>/dev/null | sed 's/^/  /'
ls -1 /d/yzd/MSYS2/var/cache/pacman/pkg 2>/dev/null | head -20 | sed 's/^/    /'

echo
echo "=== any sndfile/espeak/sonic source already on D: ? ==="
find /d/yzd -maxdepth 3 -iname '*libsndfile*' -o -maxdepth 3 -iname '*sndfile*' -o -maxdepth 3 -iname '*espeak*' -o -maxdepth 3 -iname '*sonic*' 2>/dev/null | head -30 | sed 's/^/  /'
