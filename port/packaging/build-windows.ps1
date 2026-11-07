<#
.SYNOPSIS
  Build Digimon Battle Chronicle for Windows from your own disc image.
.DESCRIPTION
  Double-click Build-Windows.bat, or run this script. Without a disc argument a file
  dialog asks for the image. The finished game is in the DigimonBattleGC folder next to
  this script, and zipped beside it as DigimonBattleGC-<version>-windows-<arch>.zip.
  If anything fails, build-report-<time>.txt next to this script says what happened.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File build-windows.ps1
  powershell -ExecutionPolicy Bypass -File build-windows.ps1 "D:\Games\Digimon Battle Chronicle (Japan).rvz" -Portable
.NOTES
  Uses MSYS2 (https://www.msys2.org) for the clang toolchain and SDL2; offers to install
  it with winget when it is missing.
#>
param(
    [Parameter(Position = 0)][string]$Disc,
    [switch]$Portable,
    [switch]$NoZip,
    [string]$Msys2 = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

function Write-Report([string]$step, [string]$message) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $path = Join-Path $PSScriptRoot "build-report-$stamp.txt"
    $version = if (Test-Path (Join-Path $PSScriptRoot 'VERSION')) { (Get-Content (Join-Path $PSScriptRoot 'VERSION') -Raw).Trim() } else { 'dev' }
    @(
        'Digimon Battle Chronicle self-build kit: build report (Windows setup)'
        "kit version : $version"
        "time        : $(Get-Date -Format s)"
        "failed step : $step"
        "error       : $message"
        "system      : $([Environment]::OSVersion.VersionString) $env:PROCESSOR_ARCHITECTURE"
        "powershell  : $($PSVersionTable.PSVersion)"
        "msys2       : $Msys2 (exists: $(Test-Path $Msys2))"
    ) | Set-Content -LiteralPath $path -Encoding UTF8
    Write-Host "A build report was written to $path" -ForegroundColor Yellow
}

function Stop-Build([string]$step, [string]$message) {
    Write-Host "ERROR: $message" -ForegroundColor Red
    Write-Report $step $message
    exit 1
}

$bash = Join-Path $Msys2 'usr\bin\bash.exe'
if (-not (Test-Path $bash)) {
    Write-Host "MSYS2 was not found at $Msys2. It provides the compiler and SDL2 this build needs." -ForegroundColor Yellow
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    if ($winget) {
        $answer = Read-Host 'Install it now with winget (about 500 MB)? [Y/n]'
        if ($answer -eq '' -or $answer -match '^[Yy]') {
            & winget install --id MSYS2.MSYS2 -e --accept-package-agreements --accept-source-agreements
            if (-not (Test-Path $bash)) { Stop-Build 'installing MSYS2' "winget finished but $bash is still missing; install MSYS2 from https://www.msys2.org and run this again." }
        } else {
            Stop-Build 'installing MSYS2' 'MSYS2 is required: winget install MSYS2.MSYS2 (or https://www.msys2.org)'
        }
    } else {
        Stop-Build 'installing MSYS2' 'MSYS2 is required: install it from https://www.msys2.org, then run this again.'
    }
}

if (-not $Disc) {
    # A native dialog; build.py would otherwise look in the usual folders itself.
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = 'Choose your Digimon Battle Chronicle (Japan) disc image'
        $dialog.Filter = 'GameCube disc image (*.iso;*.gcm;*.rvz;*.gcz;*.ciso)|*.iso;*.gcm;*.rvz;*.gcz;*.ciso|All files (*.*)|*.*'
        $dialog.InitialDirectory = Join-Path $env:USERPROFILE 'Downloads'
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $Disc = $dialog.FileName }
        else { Write-Host 'No disc chosen.'; exit 1 }
    } catch {
        Write-Host 'No file dialog available; build.py will look for the disc itself.'
    }
}
$discPath = ''
if ($Disc) {
    if (-not (Test-Path -LiteralPath $Disc)) { Stop-Build 'choosing the disc image' "$Disc does not exist" }
    $discPath = (Resolve-Path -LiteralPath $Disc).Path
}

Write-Host 'Installing/updating the MSYS2 CLANG64 build tools (clang, cmake, ninja, SDL2, python)...'
$env:MSYSTEM = 'CLANG64'
$env:CHERE_INVOKING = '1'
& $bash -lc 'pacman -S --needed --noconfirm mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja mingw-w64-clang-x86_64-SDL2 mingw-w64-clang-x86_64-pkgconf mingw-w64-clang-x86_64-python mingw-w64-clang-x86_64-python-zstandard'
if ($LASTEXITCODE) { Stop-Build 'installing the MSYS2 packages' 'pacman failed; run "pacman -Syu" in an MSYS2 shell and try again.' }

# Paths travel through the environment so spaces and non-ASCII names survive bash quoting.
# build.py writes its own report (with the full build output) if a later step fails.
$env:DBGC_DISC = $discPath
$flags = @()
if ($Portable) { $flags += '--portable' }
if ($NoZip) { $flags += '--no-zip' }
$env:DBGC_FLAGS = $flags -join ' '
& $bash -lc 'if [ -n "$DBGC_DISC" ]; then python build.py "$DBGC_DISC" $DBGC_FLAGS; else python build.py $DBGC_FLAGS; fi'
$code = $LASTEXITCODE
if ($code -eq 0) {
    Write-Host ''
    Write-Host 'Finished. The game is in the DigimonBattleGC folder.' -ForegroundColor Green
    Start-Process explorer.exe -ArgumentList "`"$(Join-Path $PSScriptRoot 'DigimonBattleGC')`""
}
exit $code
