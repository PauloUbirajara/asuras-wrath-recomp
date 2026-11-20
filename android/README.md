# Android build

Two activities:

- **`LauncherActivity`:** the launcher, in Kotlin with Material layouts. It has the game and Files folders, the shader cache (built on the phone or imported), the main settings, and Play. It edits the same `launcher.cfg` the game reads, and loads no native code.
- **`GameActivity`:** SDL's `SDLActivity` running the same native code as the desktop build (`libasura_wrath.so`), in a process of its own. It starts straight into the game, full screen. Quitting ends that process, so each Play starts fresh.

Gradle builds the repo's `CMakeLists.txt` with the NDK for each ABI and packs `libasura_wrath.so`, `librexruntime.so` and the GPU plugins into the APK.

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

Gradle leaves the APKs in `android/app/build/outputs/apk/release/`, signed with the debug key. For sharing, copy them to `out/package/` with the version in the name and a `SHA256SUMS-android`:

```bash
tools/package.sh --android
```

| File | For |
|---|---|
| `…-android-universal.apk` | Any 64-bit device (both ABIs, larger) |
| `…-android-arm64-v8a.apk` | Phones and tablets |
| `…-android-x86_64.apk` | Emulators, x86 Chromebooks |

The first build also cross-builds DXC, the shader compiler `mantra_shadergen` uses, which has no prebuilt Android version: `tools/build_dxc_android.sh` fetches the prebuilt's version (v1.8.2407) into `out/dxc/` and builds `libdxcompiler.so` per ABI, about 8 minutes each. Later builds reuse it. `mantra_shadergen` goes in the APK as `lib/<abi>/libmantra_shadergen.so`, since Android only runs programs from an app's library folder.

32-bit devices aren't supported: the game's memory doesn't fit a 32-bit address space. `ASURA_ABIS=arm64-v8a ./build.sh --android` builds one ABI only, which is faster.

## On the device

- **Requirements:** Android 10 or newer, arm64 or x86_64, Vulkan 1.1.
- **Game files:** copy the extracted game (the folder with `default.xex`) anywhere on the phone or SD card, then pick it with **Game › Choose folder**. The first pick asks for Android's "All files access", because the game reads its files by path. A game copied to `Android/data/org.asurawrath.recompiled/files/extracted/` is found without picking it and needs no permission.
- **Shader cache:** **Shader cache › Build the shader cache** runs `mantra_shadergen` on the phone, in the background with a progress notification (Stop is there too; the next build continues). It takes much longer than on a computer, so keep the phone charging. Or build it on any computer (Game data › Shaders) and use **Import**: the cache is SPIR-V, the same for every CPU.
- **Files:** `launcher.cfg` is in `Android/data/org.asurawrath.recompiled/files/`. Saves, the shader cache and logs go in `asura_wrath_recomp/` next to it, or the folder picked under **Files folder**.
- **In game:** Back opens the in-game menu: the launcher's pages over the paused game (InGameMenuActivity), with Resume, Restart and Quit. Settings, the button remap and the controller setup apply when you go back to the game. A Restart (for settings that need one) ends the game, and the launcher starts it again. The desktop's in-game menu (ImGui) isn't used on Android.
- **Controller setup:** Controls › Controller setup, for a controller the game reads wrongly (often the triggers moving the camera and the right stick doing nothing). Tap a control and press it on the controller (10 seconds), or Start setup to go through them all. The prompts give the Xbox and PlayStation names (LT / L2, LB / L1, L3, R3). It records the input SDL will see (SDL's Android button and axis numbering, `SdlPad.kt`) and saves it per controller model in `controller_setup_android.txt`; the game adds it as an SDL mapping for that controller.
- **Button remap:** Controls › Controller: tap a button's row, then press the controller button that should act as it.
- **Touch controls:** shown over the game, hidden while a controller is connected (Controls › Touch controls). Hide, between Back and Start at the top, hides the others until tapped again. Controls › Touch layout › Edit layout opens them full screen: drag one to move it, the slider sets its size.
- **GPU driver:** Snapdragon phones can load a custom Vulkan driver .zip (Settings › GPU driver, through libadrenotools).
- **Not here yet:** extracting an `.iso` (do it on a computer).

## Back button

In game, Back (or Esc, or the controller's menu bind) opens the in-game menu. In the menu, Back closes whatever is on top (the drawer, a section) and then resumes the game.

## Icons

The launcher icons live in `app/src/main/res/mipmap-*/ic_launcher*`: adaptive layers (background, foreground, monochrome) for Android 8+, plus a flat PNG for older launchers. The 512 px store icon is `app/src/main/ic_launcher-playstore.png`.

## Vulkan fallbacks

Both fallbacks can be forced on desktop for testing:

- **No dynamic rendering:** the renderer falls back to render passes. Force it with `MANTRA_FORCE_RENDER_PASS=1`.
- **No BC texture formats:** textures are decoded to RGBA8, R8 or RG8 on load. Force it with `MANTRA_FORCE_BC_DECODE=1`.
