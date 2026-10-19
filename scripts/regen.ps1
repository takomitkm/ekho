# Re-derive the Linux port from pristine upstream, then apply every post-strip
# fix. Runs on the dev machine (Windows paths), not in WSL.
#   $PSScriptRoot\..\scripts\regen.ps1
param(
    [string]$Repo = 'S:\work\WCIS\repos\ekho-linux',
    [string]$Pristine = 'C:\Users\takomi\AppData\Local\Temp\opencode\ekho-patched',
    [string]$Tools = 'C:\Users\takomi\AppData\Local\Temp\opencode'
)
$ErrorActionPreference = 'Stop'
$py = 'C:\Users\takomi\AppData\Local\Programs\Python\Python313\python.exe'

Write-Output '=== 1. stash modules that are not in the pristine tree ==='
$stash = Join-Path $Tools 'ekho-extra'
. C:\Users\takomi\.config\opencode\bin\recycle.ps1
if (Test-Path $stash) { Remove-ToRecycleBin $stash | Out-Null }
New-Item -ItemType Directory -Force -Path $stash | Out-Null
$added = 'text_normalize.h','text_normalize.cpp','streaming.h','streaming.cpp',
         'alsa_backend.h','alsa_backend.cpp','ekho_stream_main.cpp'
foreach ($f in $added) {
    if (Test-Path "$Repo\src\$f") { Copy-Item "$Repo\src\$f" $stash -Force }
}
Write-Output "  stashed $((Get-ChildItem $stash).Count) files"

Write-Output ''
Write-Output '=== 2. restore src/ from pristine upstream ==='
Get-ChildItem "$Repo\src" -File | Remove-Item -Force
& robocopy.exe "$Pristine\src" "$Repo\src" *.cpp *.h /NFL /NDL /NJH /NJS /NP | Out-Null
foreach ($f in 'mingw_compat.h','stdafx.h','gtk2.cpp') {
    if (Test-Path "$Repo\src\$f") { Remove-Item "$Repo\src\$f" -Force }
}
Write-Output "  $((Get-ChildItem "$Repo\src" -File).Count) files"

Write-Output ''
Write-Output '=== 3. strip windows branches + posix-ise the compat shims ==='
& $py "$Tools\strip2.py"
& $py "$Tools\fix_inc.py"    | Out-Null
& $py "$Tools\fix_posix.py"

Write-Output ''
Write-Output '=== 4. restore the added modules ==='
Copy-Item "$stash\*" "$Repo\src\" -Force
foreach ($f in $added) {
    if (-not (Test-Path "$Repo\src\$f")) { Write-Output "  MISSING: $f" }
}

Write-Output ''
Write-Output '=== 5. guard the remaining libcurl uses ==='
& $py "$Tools\guard_curl.py"

Write-Output ''
Write-Output '=== 6. uninitialised TextType in Word::split ==='
$w = "$Repo\src\word.cpp"
$t = [IO.File]::ReadAllText($w)
if ($t -match '(?m)^  TextType type;$') {
    $t = $t -replace '(?m)^  TextType type;$', @"
  // gcc cannot prove every path that reaches a push_back() assigns this, and
  // reading an uninitialised enum is undefined behaviour, not a warning.
  TextType type = PHONETIC;
"@
    [IO.File]::WriteAllText($w, $t, (New-Object System.Text.UTF8Encoding($false)))
    Write-Output '  patched'
} else {
    Write-Output '  already initialised'
}

Write-Output ''
Write-Output '=== 7. structural check ==='
& $py "$Tools\balance.py" "$Repo\src"

Write-Output ''
Write-Output '=== 8. windows leftovers ==='
$hits = Select-String -Path "$Repo\src\*.cpp","$Repo\src\*.h" `
    -Pattern 'stdafx|mingw_compat|ekho_access|ekho_mkdir|ekho_bzero|ekho_setenv'
if ($hits) {
    $hits | Select-Object -First 8 | ForEach-Object { '  {0}:{1}  {2}' -f $_.Filename,$_.LineNumber,$_.Line.Trim() }
} else {
    Write-Output '  clean'
}