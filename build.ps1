# Builds mdview.exe and setup.exe (the installer, which embeds mdview.exe) with Open Watcom.
#   .\build.ps1                      both; the installer also lands in dist\mdview-setup-<version>.exe
#   .\build.ps1 mdview               just the viewer
# Open Watcom is found through the WATCOM environment variable (default C:\watcom).
param([ValidateSet("all", "mdview", "setup")][string]$Name = "all")

$wat = if ($env:WATCOM) { $env:WATCOM } else { "C:\watcom" }
$env:WATCOM = $wat
$env:PATH = "$wat\binnt64;$wat\binnt;$env:PATH"
$env:INCLUDE = "$wat\h;$wat\h\nt"
$env:LIB = "$wat\lib386;$wat\lib386\nt"

function Build($n) {
    wcl386 -zq -bt=nt -3s -ox "$n.c" "-fe=$n.exe" "-l=nt_win"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    wrc -q -bt=nt -r "-i=$wat\h;$wat\h\nt" "$n.rc"          # icon + version info
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    wrc -q -bt=nt "$n.res" "$n.exe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Remove-Item "$n.res"
}

Push-Location $PSScriptRoot
try {
    # version = build time, e.g. 2026.10.31.1423; the installer reuses it
    $now = Get-Date
    $parts = @($now.Year, $now.Month, $now.Day, [int]$now.ToString("HHmm"))
    "#define VER_STR `"$($parts -join '.')`"`r`n#define VER_COMMA $($parts -join ',')`r`n" |
        Set-Content -Encoding ASCII version.h

    Build "mdview"
    Build "setup"
    $ver = ((Get-Content version.h | Select-String 'VER_STR "([^"]+)"').Matches[0].Groups[1].Value)
    New-Item -ItemType Directory -Force dist | Out-Null
    Copy-Item setup.exe "dist\mdview-setup-$ver.exe" -Force
    "dist\mdview-setup-$ver.exe"
} finally { Pop-Location }
