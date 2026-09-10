#!/usr/bin/env bash
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "[+] =========================================="
echo "[+] Building Asura's Wrath Recompiled (Android)"
echo "[+] =========================================="

git submodule update --init --recursive

# Check ANDROID_NDK environment variable
if [ -z "$ANDROID_NDK" ]; then
    echo "[-] Error: ANDROID_NDK environment variable is not set."
    echo "[-] Please set ANDROID_NDK to your NDK installation path."
    exit 1
fi

if [ -z "$ANDROID_HOME" ]; then
    NDK_PARENT="$(cd "$ANDROID_NDK/../.." 2>/dev/null && pwd || true)"
    if [ -d "$NDK_PARENT/platforms" ]; then
        ANDROID_HOME="$NDK_PARENT"
    elif [ -d "$HOME/Android/Sdk" ]; then
        ANDROID_HOME="$HOME/Android/Sdk"
    fi
fi

mkdir -p "$REPO_ROOT/android"
{
    if [ -n "$ANDROID_HOME" ]; then
        echo "sdk.dir=$ANDROID_HOME"
    fi
} > "$REPO_ROOT/android/local.properties"

BUILD_TYPE="assembleRelease"
CMAKE_EXTRA_ARGS=""
GRADLE_FLAGS=()

for arg in "$@"; do
    if [ "$arg" = "--debug" ] || [ "$arg" = "-d" ]; then
        BUILD_TYPE="assembleDebug"
    elif [[ "$arg" == -D* ]]; then
        CMAKE_EXTRA_ARGS="$CMAKE_EXTRA_ARGS $arg"
    else
        GRADLE_FLAGS+=("$arg")
    fi
done

if [ -n "$CMAKE_EXTRA_ARGS" ]; then
    GRADLE_FLAGS+=("-PcmakeArgs=$CMAKE_EXTRA_ARGS")
fi

cd "$REPO_ROOT/android"
chmod +x gradlew
./gradlew "$BUILD_TYPE" "${GRADLE_FLAGS[@]}"

mkdir -p "$REPO_ROOT/out/dist"
APK_PATH=$(find "$REPO_ROOT/android/app/build/outputs/apk" -name "*.apk" 2>/dev/null | head -n 1)

if [ -n "$APK_PATH" ]; then
    cp "$APK_PATH" "$REPO_ROOT/out/dist/asura_wrath_recomp_android.apk"
    echo "[+] APK created successfully: $REPO_ROOT/out/dist/asura_wrath_recomp_android.apk"
else
    echo "[+] Build finished."
fi
