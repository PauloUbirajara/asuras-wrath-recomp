@echo off
setlocal enabledelayedexpansion

set REPO_ROOT=%~dp0
if "%REXSDK_DIR%"=="" set REXSDK_DIR=%REPO_ROOT%tools\rexglue

echo [+] ============================================
echo [+] Building Asura's Wrath Recompiled (Windows)
echo [+] ============================================

git submodule update --init --recursive

cmake --preset win-amd64-release
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

cmake --build out/build/win-amd64-release --parallel
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

echo [+] Build completed successfully: out\build\win-amd64-release\asura_wrath_recomp.exe
