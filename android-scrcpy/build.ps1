$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed (exit $LASTEXITCODE)" }
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { throw "C++ compilation failed (exit $LASTEXITCODE)" }
$exe = Join-Path $root "build\Release\android-scrcpy.exe"
if (-not (Test-Path $exe)) { throw "Build finished but EXE was not found: $exe" }
Write-Host "Build successful: $exe" -ForegroundColor Green
Write-Host "Place the official Android SDK platform-tools folder next to the EXE before running."
