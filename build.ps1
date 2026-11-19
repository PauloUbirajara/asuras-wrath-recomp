# Build Asura's Wrath Recompiled on Windows (PowerShell).
#
#   .\build.ps1                 release build for your CPU (-march=native)
#   .\build.ps1 -Config debug   debug build (also: relwithdebinfo)
#   .\build.ps1 -Portable       -msse4.1 baseline, runs on any SSE4.1 CPU (for sharing)
#   .\build.ps1 -Codegen        force a full rexglue codegen re-run
#   .\build.ps1 -Clean          wipe the build directory first
#
# Needs: Visual Studio 2022 Build Tools (the Windows SDK and its C runtime),
# LLVM clang 20+ on PATH, CMake 3.25+, Ninja, Git, and the Vulkan SDK. Run it
# from a "Developer PowerShell for VS 2022" (x64), so clang finds the SDK.
# CPU flags: as build.sh, passed to everything, SDK included.
param(
  [ValidateSet("release", "debug", "relwithdebinfo")] [string]$Config = "release",
  [switch]$Portable,
  [switch]$Codegen,
  [switch]$Clean
)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

foreach ($tool in "cmake", "ninja", "clang++", "git") {
  if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
    throw "$tool is not on PATH (see the requirements at the top of build.ps1)"
  }
}

$Preset = "win-amd64-$Config"
$BuildDir = Join-Path $PSScriptRoot "out\build\$Preset"

# --- CPU flags ---------------------------------------------------------------
$CpuFlags = "-msse4.1"
if (-not $Portable) {
  $null = "" | clang++ -march=native -x c++ -E - 2>$null
  if ($LASTEXITCODE -eq 0) { $CpuFlags = "-march=native" }
  else { Write-Host "[!] clang++ does not accept -march=native; using the portable baseline" }
}
Write-Host "[+] Preset: $Preset, CPU flags: $CpuFlags"

# --- Submodule ---------------------------------------------------------------
if (-not (Test-Path "thirdparty\rexglue\CMakeLists.txt")) {
  git submodule update --init --recursive
  if ($LASTEXITCODE -ne 0) { throw "git submodule update failed" }
}

# --- Codegen -----------------------------------------------------------------
# The CMake build runs codegen itself when its inputs change; -Codegen drops
# the stamp to force a full re-run.
if ($Codegen) {
  Remove-Item -ErrorAction SilentlyContinue generated\default\codegen.build.stamp, generated\default\codegen.stamp
}

# --- Configure & build -------------------------------------------------------
if ($Clean -and (Test-Path $BuildDir)) { Remove-Item -Recurse -Force $BuildDir }
cmake --preset $Preset "-DCMAKE_C_FLAGS=$CpuFlags" "-DCMAKE_CXX_FLAGS=$CpuFlags"
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
cmake --build $BuildDir --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "[+] Built: $BuildDir\asura_wrath.exe"
