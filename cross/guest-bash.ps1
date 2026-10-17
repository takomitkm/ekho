<#
.SYNOPSIS
    Run a local shell script on the school guest, with its MSYS2 bash.

.DESCRIPTION
    The guest's default ssh shell is cmd, and cmd cannot run a .sh file. This
    uploads the script into C:\Users\YiZhidi\ first -- which also keeps the
    guest's copy current, so the guest always has a working backup of the
    scaffolding -- then hands it to D:\yzd\MSYS2\usr\bin\bash.exe explicitly.

    Before uploading, each script is checked for non-ASCII bytes, bare CR, and
    a BOM. The guest console is GBK, so a script with Chinese in it can fail
    there in ways that make no sense from this side; the check refuses the run
    unless -NoCheck is given. (A clean script is ASCII, LF, no BOM.)

    Output streams as it arrives. Several scripts can be given; they run in
    order and the exit code is the last one's.

        .\guest-bash.ps1 -Path .\x-pack-kit.sh
        .\guest-bash.ps1 -Path .\x-common.sh, .\x-kit-all.sh

.PARAMETER Path
    One or more local .sh files.

.PARAMETER RemoteDir
    Where on the guest the scripts are kept. Default C:/Users/YiZhidi.

.PARAMETER Guest
    ssh alias for the guest. Default school-guest-lan.

.PARAMETER NoCheck
    Skip the ASCII/LF/BOM check. Use only when a script really does need
    non-ASCII, and expect the guest to be the judge of whether it works.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string[]] $Path,

    [string] $RemoteDir = 'C:/Users/YiZhidi',
    [string] $Guest     = 'school-guest-lan',

    [switch] $NoCheck
)

$ErrorActionPreference = 'Stop'

# Forward slashes work through cmd for the executable, but backslashes are what
# cmd itself would write, so use those.
$bash = 'D:\yzd\MSYS2\usr\bin\bash.exe'
$rc = 0

function Test-Script {
    param([string] $File)
    $b = [System.IO.File]::ReadAllBytes($File)
    $problems = @()
    $first = -1
    for ($i = 0; $i -lt $b.Length; $i++) {
        if ($b[$i] -gt 126 -or $b[$i] -eq 13) { $first = $i; break }
    }
    if ($first -ge 0) {
        $what = if ($b[$first] -eq 13) { 'bare CR' } else { 'non-ASCII byte 0x{0:X2}' -f $b[$first] }
        # Say where, so the fix is obvious.
        $line = 1
        for ($i = 0; $i -lt $first; $i++) { if ($b[$i] -eq 10) { $line++ } }
        $problems += "$what at offset $first (line $line)"
    }
    if ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) {
        $problems += 'UTF-8 BOM'
    }
    return $problems
}

foreach ($p in $Path) {
    if (-not (Test-Path -LiteralPath $p)) { throw "no such script: $p" }
    $leaf = Split-Path -Leaf $p

    if (-not $NoCheck) {
        $problems = Test-Script -File $p
        if ($problems.Count -gt 0) {
            Write-Host "[guest-bash] $leaf is not ASCII/LF/BOM-clean:" -ForegroundColor Red
            foreach ($w in $problems) { Write-Host "    $w" -ForegroundColor Red }
            Write-Host "  The guest console is GBK; scripts must stay ASCII-only." -ForegroundColor Red
            Write-Host "  Fix it, or pass -NoCheck to send it anyway." -ForegroundColor Red
            exit 2
        }
    }

    Write-Host "[guest-bash] scp  $leaf  ->  ${Guest}:${RemoteDir}/$leaf"
    scp $p "${Guest}:${RemoteDir}/$leaf"
    if ($LASTEXITCODE -ne 0) { throw "scp failed for $leaf (exit $LASTEXITCODE)" }

    # MSYS2 bash wants the POSIX form of the same path: C:/Users -> /c/Users.
    if ($RemoteDir -match '^([A-Za-z]):(.*)$') {
        $remotePath = '/' + $Matches[1].ToLower() + $Matches[2] + "/$leaf"
    } else {
        $remotePath = "$RemoteDir/$leaf"
    }
    Write-Host "[guest-bash] run  $bash --login $remotePath"
    ssh $Guest "$bash --login $remotePath"
    $rc = $LASTEXITCODE
    Write-Host "[guest-bash] exit $rc"
}

exit $rc
