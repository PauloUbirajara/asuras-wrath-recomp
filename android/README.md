# Android build

A thin Kotlin app (`MainActivity`, a subclass of SDL's `SDLActivity`) that runs the same native code as the desktop build. Gradle builds the repo's `CMakeLists.txt` with the NDK for arm64 and packs `libasura_wrath.so`, `librexruntime.so` and the GPU plugins into the APK.

## Requirements

- JDK 17 or newer
- Android SDK in `$ANDROID_HOME` (default `~/Android/Sdk`) with:
  - `platforms;android-35`
  - `build-tools;35.0.0`
  - `ndk;28.2.13676358`
- CMake 3.25 or newer on `PATH`
- Ninja

## Build

```bash
./build.sh --android
```

The APK lands in `android/app/build/outputs/apk/release/`. It is signed with the debug key.

## On the device

- **Requirements:** Android 10 or newer, arm64, Vulkan 1.1.
- **Game files:** copy the extracted game (the folder with `default.xex`) to `Android/data/org.asurawrath.recompiled/files/extracted/`. The launcher finds it there on first start.
- **Shader cache:** build it on the desktop, then copy `mantra_shaders.mshc` into `Android/data/org.asurawrath.recompiled/files/asura_wrath_recomp/cache/`. `mantra_shadergen` doesn't run on Android.
- **Play:** the game starts in the launcher's own process. Settings that need a restart apply the next time the app opens.

## Vulkan fallbacks

Both fallbacks can be forced on desktop for testing:

- **No dynamic rendering:** the renderer falls back to render passes. Force it with `MANTRA_FORCE_RENDER_PASS=1`.
- **No BC texture formats:** textures are decoded to RGBA8, R8 or RG8 on load. Force it with `MANTRA_FORCE_BC_DECODE=1`.
