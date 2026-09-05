@echo off
setlocal enabledelayedexpansion

set REPO_ROOT=%~dp0
if "%REXSDK_DIR%"=="" set REXSDK_DIR=%REPO_ROOT%tools\rexglue

set PACKAGE=false
for %%A in (%*) do (
    if "%%A"=="--package" set PACKAGE=true
    if "%%A"=="--zip" set PACKAGE=true
    if "%%A"=="-p" set PACKAGE=true
)

echo [+] ============================================
echo [+] Building Asura's Wrath Recompiled (Windows)
echo [+] ============================================

git submodule update --init --recursive

cmake --preset win-amd64-release -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D12=ON
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

cmake --build out/build/win-amd64-release --parallel
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

echo [+] Build completed successfully: out\build\win-amd64-release\asura_wrath_recomp.exe

if "!PACKAGE!"=="true" (
    echo [+] Packaging release bundle...
    set "DIST_ARCH=%PROCESSOR_ARCHITECTURE%"
    if "!DIST_ARCH!"=="AMD64" set "DIST_ARCH=x86_64"
    set "DIST_NAME=asura_wrath_recomp_windows_!DIST_ARCH!"
    set "DIST_DIR=%REPO_ROOT%out\dist\!DIST_NAME!"
    set "ZIP_PATH=%REPO_ROOT%out\dist\!DIST_NAME!.zip"

    if exist "!DIST_DIR!" rmdir /s /q "!DIST_DIR!"
    if exist "!ZIP_PATH!" del /f /q "!ZIP_PATH!"
    if not exist "%REPO_ROOT%out\dist" mkdir "%REPO_ROOT%out\dist"
    mkdir "!DIST_DIR!"

    copy /y "%REPO_ROOT%out\build\win-amd64-release\asura_wrath_recomp.exe" "!DIST_DIR!\" >nul
    if exist "%REPO_ROOT%out\build\win-amd64-release\*.dll" (
        copy /y "%REPO_ROOT%out\build\win-amd64-release\*.dll" "!DIST_DIR!\" >nul
    )
    if exist "%REPO_ROOT%tools\rexglue\out\win-amd64\*.dll" (
        copy /y "%REPO_ROOT%tools\rexglue\out\win-amd64\*.dll" "!DIST_DIR!\" >nul
    )

    powershell -Command "Compress-Archive -Path '!DIST_DIR!' -DestinationPath '!ZIP_PATH!' -Force"

    echo [+] Release bundle created at: !ZIP_PATH!
)
