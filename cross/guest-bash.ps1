<#
.SYNOPSIS
    Run a local shell script on the school guest, with its MSYS2 bash.

.DESCRIPTION
    The guest's default ssh shell is cmd, and cmd cannot run a .sh file. This
    uploads the script into C:\Users\YiZhidi\ first -- which also keeps the
    guest's copy current, so the guest always has a working backup of the
    scaffolding -- then hands it to D:\yzd\MSYS2\usr\bin\bash.exe explicitly.

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
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string[]] $Path,

    [string] $RemoteDir = 'C:/Users/YiZhidi',
    [string] $Guest     = 'school-guest-lan'
)

$ErrorActionPreference = 'Stop'

# Forward slashes work through cmd for the executable, but backslashes are what
# cmd itself would write, so use those.
$bash = 'D:\yzd\MSYS2\usr\bin\bash.exe'
$rc = 0

foreach ($p in $Path) {
    if (-not (Test-Path -LiteralPath $p)) { throw "no such script: $p" }
    $leaf = Split-Path -Leaf $p

    Write-Host "[guest-bash] scp  $leaf  ->  ${Guest}:${RemoteDir}/$leaf"
    scp $p "${Guest}:${RemoteDir}/$leaf"
    if ($LASTEXITCODE -ne 0) { throw "scp failed for $leaf (exit $LASTEXITCODE)" }

    # MSYS2 bash wants the POSIX form of the same path.
    $remotePath = "/c/Users/YiZhidi/$leaf"
    Write-Host "[guest-bash] run  $bash --login $remotePath"
    ssh $Guest "$bash --login $remotePath"
    $rc = $LASTEXITCODE
    Write-Host "[guest-bash] exit $rc"
}

exit $rc
