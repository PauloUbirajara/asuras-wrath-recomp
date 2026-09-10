@echo off
setlocal enabledelayedexpansion

set REPO_ROOT=%~dp0

echo [+] ============================================
echo [+] Building Asura's Wrath Recompiled (Android)
echo [+] ============================================

git submodule update --init --recursive

if "%ANDROID_NDK%"=="" (
    echo [-] Error: ANDROID_NDK environment variable is not set.
    echo [-] Please set ANDROID_NDK to your NDK installation path.
    exit /b 1
)

if "%ANDROID_HOME%"=="" (
    if exist "%ANDROID_NDK%\..\..\platforms" set ANDROID_HOME=%ANDROID_NDK%\..\..
    if "%ANDROID_HOME%"=="" if exist "%LOCALAPPDATA%\Android\Sdk" set ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk
)

if not exist "%REPO_ROOT%android" mkdir "%REPO_ROOT%android"
(
    if not "%ANDROID_HOME%"=="" echo sdk.dir=%ANDROID_HOME:\=\\%
    echo ndk.dir=%ANDROID_NDK:\=\\%
) > "%REPO_ROOT%android\local.properties"

set BUILD_TYPE=assembleRelease
for %%A in (%*) do (
    if "%%A"=="--debug" set BUILD_TYPE=assembleDebug
    if "%%A"=="-d" set BUILD_TYPE=assembleDebug
)

cd /d "%REPO_ROOT%android"
call gradlew.bat %BUILD_TYPE%

if not exist "%REPO_ROOT%out\dist" mkdir "%REPO_ROOT%out\dist"
for /r "%REPO_ROOT%android\app\build\outputs\apk" %%F in (*.apk) do (
    copy /y "%%F" "%REPO_ROOT%out\dist\asura_wrath_recomp_android.apk" >nul
    echo [+] APK created successfully: %REPO_ROOT%out\dist\asura_wrath_recomp_android.apk
    goto :done
)

:done
