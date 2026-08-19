$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = Split-Path -Parent $ScriptDir
$VenvDir = Join-Path $ProjectDir ".build\gui-venv-windows"
$WorkDir = Join-Path $ProjectDir ".build\pyinstaller-windows"
$DistDir = Join-Path $ProjectDir "dist\windows"
$Python = Join-Path $VenvDir "Scripts\python.exe"
$PyInstaller = Join-Path $VenvDir "Scripts\pyinstaller.exe"

py -3 -m venv $VenvDir
& $Python -m pip install --upgrade pip
& $Python -m pip install -r (Join-Path $ProjectDir "flasher\requirements-build.txt")

Push-Location $ProjectDir
try {
    & $Python -m unittest discover -v
    & $PyInstaller `
        --clean `
        --noconfirm `
        --distpath $DistDir `
        --workpath $WorkDir `
        (Join-Path $ProjectDir "flasher\MarauderEternalFlasher.spec")
    & (Join-Path $DistDir "MarauderEternalFlasher.exe") --self-test
    Get-FileHash (Join-Path $DistDir "MarauderEternalFlasher.exe") -Algorithm SHA256 |
        Format-List | Out-File (Join-Path $DistDir "SHA256SUMS.txt")
} finally {
    Pop-Location
}

Write-Host "Windows application: $DistDir\MarauderEternalFlasher.exe"
