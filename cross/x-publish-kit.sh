#!/bin/bash
# ASCII only. Rebuild kit-ekho (now carrying licences + a source pointer) and
# pack it for the dev machine to pick up.
. /c/Users/YiZhidi/x-common.sh
set -u

echo "############ 1. kit-ekho"
bash /c/Users/YiZhidi/x-kit-ekho.sh || { echo "kit-ekho FAILED"; exit 1; }

echo
echo "############ 2. licences in the kit"
ls -la /d/yzd/ekho-x/kit-ekho/share/doc/ 2>&1 | sed 's/^/  /'
echo "  --- SOURCE-AND-LICENCES.txt ---"
sed 's/^/  | /' /d/yzd/ekho-x/kit-ekho/share/doc/SOURCE-AND-LICENCES.txt

echo
echo "############ 3. pack for download"
cd /d/yzd/ekho-x || exit 1
tar cf /c/Users/YiZhidi/kit-ekho.tar kit-ekho || { echo "tar FAILED"; exit 1; }
ls -la /c/Users/YiZhidi/kit-ekho.tar | sed 's/^/  /'
echo "  kit files: $(find kit-ekho -type f | wc -l)"
echo "  kit du:    $(du -sh kit-ekho | cut -f1)"
echo "  manifest lines: $(wc -l < kit-ekho/MANIFEST.txt)"
