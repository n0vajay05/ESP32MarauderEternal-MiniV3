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
    if ($LASTEXITCODE -ne 0) { throw "Desktop tests failed." }
    & $PyInstaller `
        --clean `
        --noconfirm `
        --distpath $DistDir `
        --workpath $WorkDir `
        (Join-Path $ProjectDir "flasher\MarauderEternalFlasher.spec")
    if ($LASTEXITCODE -ne 0) { throw "Flasher build failed." }
    & (Join-Path $DistDir "MarauderEternalFlasher.exe") --self-test
    if ($LASTEXITCODE -ne 0) { throw "Bundled self-test failed." }
    & $Python (Join-Path $ProjectDir "scripts\package_flasher.py") `
        (Join-Path $DistDir "MarauderEternalFlasher.exe") --platform windows-x86_64
    if ($LASTEXITCODE -ne 0) { throw "Flasher release packaging failed." }
} finally {
    Pop-Location
}

Write-Host "Windows application: $DistDir\MarauderEternalFlasher.exe"
