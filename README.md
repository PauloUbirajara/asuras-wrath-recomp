# Asura's Wrath Recompiled

PC port of Asura's Wrath (`default.xex`, Title ID `43430817`) via Xbox 360 static PowerPC recompilation to C++23, rendered by the in-repo Vulkan renderer `mantra`.

> Note: made and tested on Linux. A Windows build can be made (natively or cross-compiled from Linux). An **experimental Android** build exists (arm64; touch controls or a controller; see [Android](#android-experimental)).

<a href="https://www.youtube.com/playlist?list=PLVyuezqTsTB0">
        <img alt="cover" src="https://github.com/user-attachments/assets/339f7f1b-85d4-48ac-9b5f-ac4fad276dad" />
</a>

https://www.youtube.com/playlist?list=PLVyuezqTsTB0

> Original illustration by Satoshi Sakai

---

## TL;DR (first run)

1. Have your copy of the game ISO ready.
2. Build (`./build.sh`, see [Build](#3-build)) and start `asura_wrath`: the launcher opens.
3. **Game data > Folders**: drop the `.iso` on the window (or pick it with *Disc image...*) and let it extract.
4. **Game data > Shaders**: build the shader cache (about 10 minutes, once).
5. **Play**.

On Android: install the APK, copy the extracted game to the phone, pick it in the launcher, build or import the shader cache, then **Play** (see [Android](#android-experimental)).

---

## 1. Requirements

- A copy of the Xbox 360 game ISO (`Asura's Wrath.iso`).
- Optional: [extract-xiso](https://github.com/XboxDev/extract-xiso) to extract the `.iso` yourself (the launcher can also extract it).
- Linux: clang-20, CMake 3.25+, Ninja, glslc (shaderc), Vulkan drivers, Git.
- Windows (native build): Visual Studio 2022 Build Tools (Windows SDK + C runtime), LLVM clang 20+ on PATH, CMake 3.25+, Ninja, Git, the Vulkan SDK.
- Windows (cross build from Linux): the Linux requirements plus lld-20 and llvm-20 (`llvm-rc`, `llvm-ar`, `llvm-mt`); `tools/setup_windows_cross.sh` downloads Microsoft's SDK and runtime (see below).
- Android (built on Linux): JDK 17+, the Android SDK (`platforms;android-35`, `build-tools;35.0.0`, `ndk;28.2.13676358`) in `$ANDROID_HOME` or `~/Android/Sdk`, CMake 3.25+, Ninja (details in [android/README.md](android/README.md)).

---

## 2. Setup

### Clone Repository
```bash
git clone --recursive https://github.com/PauloUbirajara/asuras-wrath-recomp.git
cd asuras-wrath-recomp
git -C thirdparty/XenosRecomp submodule update --init --recursive
```

---

## 3. Build

> Requires the .iso contents in `extracted/` to build (the first build recompiles `extracted/default.xex`).

```bash
mkdir -p extracted/
extract-xiso -x asura_wrath.iso -d extracted/
```

### Option A: Using CMake Presets (Linux)

```bash
cmake --preset linux-amd64-release
cmake --build out/build/linux-amd64-release --parallel
```

### Option B: Using Build Scripts (Linux)

```bash
# Build binary (out/build/linux-amd64-release/)
./build.sh

# Build for any x86-64 CPU with SSE4.1 (the default is tuned for your CPU: -march=native)
./build.sh --portable

# Other options: --debug, --codegen, --clean (./build.sh --help)
```

### Option C: Windows, native (PowerShell)

Run from a *Developer PowerShell for VS 2022* (x64), so clang finds the Windows SDK:

```powershell
# Build binary (out\build\win-amd64-release\)
.\build.ps1

# Portable build (for sharing); other options: -Config debug|relwithdebinfo, -Codegen, -Clean
.\build.ps1 -Portable
```

### Option D: Windows, cross-compiled from Linux

Builds `asura_wrath.exe` on Linux with clang-20 targeting the MSVC ABI and lld-link. Microsoft's Windows SDK and C/C++ runtime can't be redistributed, so a script downloads them (through [xwin](https://github.com/Jake-Shadle/xwin)) into `out/toolchain/xwin`; running it accepts [Microsoft's license](https://go.microsoft.com/fwlink/?LinkId=2086102).

```bash
# Once: Windows SDK 10.0.22621 + MSVC runtime (~600 MB; --debug-libs for Debug builds)
tools/setup_windows_cross.sh

# The Linux build first: the Windows build runs its codegen tool
./build.sh

# Windows build (out/build/win-amd64-release-cross/); --portable for sharing, --debug
./build.sh --windows --portable
```

The same with presets: `cmake --preset win-amd64-release-cross` then `cmake --build out/build/win-amd64-release-cross --parallel` (toolchain file `cmake/windows-cross.cmake`; `XWIN_DIR` and `LLVM_SUFFIX` override the SDK folder and the `-20` suffix).

### Option E: Android (experimental), from Linux

Gradle builds the same CMake project with the NDK, plus a Kotlin launcher (see [android/README.md](android/README.md)).

```bash
# APKs in android/app/build/outputs/apk/release/ (universal, arm64-v8a, x86_64)
./build.sh --android

# One ABI only (faster)
ASURA_ABIS=arm64-v8a ./build.sh --android
```

The first build also cross-builds DXC for the on-phone shader builder (about 8 minutes per ABI); later builds reuse it.

### Package

```bash
# A release: portable build, then the archive + AppImage named after the version
./build.sh --portable
tools/package.sh --appimage --clean --version 0.1.0

# out/package/
# ├── asuras-wrath-recomp-0.1.0-linux-x86_64.tar.gz
# ├── asuras-wrath-recomp-0.1.0-linux-x86_64.AppImage
# └── SHA256SUMS-linux
```

Without `--version`, the files are named after the git tag on HEAD (`v0.1.0` → `0.1.0`), else the short commit; uncommitted changes only print a warning. `--clean` deletes earlier packages in `out/package`; the staging folders are always removed. The AppImage needs appimagetool (`APPIMAGETOOL=<path>`, PATH, or `../appimagetool/appimagetool`), which downloads its runtime unless `APPIMAGE_RUNTIME=<file>` is set.

Windows packages are a `.zip` (binaries, `assets/`, a `README.txt`):

```bash
# Cross build on Linux: out/package/asuras-wrath-recomp-<version>-windows-x86_64.zip + SHA256SUMS-windows
./build.sh --windows --portable
tools/package.sh --windows --version 0.1.0
```

```powershell
# Native build on Windows: out\package\asuras-wrath-recomp-<version>-windows-x86_64.zip + SHA256SUMS.txt
.\build.ps1 -Portable
tools\package.ps1 -Version 0.1.0
```

Android packages are the APKs, copied with the version in the name:

```bash
# out/package/asuras-wrath-recomp-<version>-android-{universal,arm64-v8a,x86_64}.apk + SHA256SUMS-android
./build.sh --android
tools/package.sh --android --version 0.1.0
```

Everything at once (Linux tar.gz + AppImage, Windows zip, APKs), from the builds already made:

```bash
./build.sh --portable && ./build.sh --windows --portable && ./build.sh --android
tools/package.sh --all --clean --version 0.1.0
```

`tools/package.sh` refuses a Windows or Android build older than the sources, so a failed build isn't packaged under the new version.

The Windows build links the Microsoft C++ runtime dynamically: players without it install the [Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe) (the zip's README says so).

For a GitHub release, attach the files in `out/package`. The default build is tuned for your CPU and may not run on others, so share only `--portable` builds. Players supply their own game: never attach game files or a shader cache built from them.

---

## 4. Game Setup

The launcher (first window when starting the game) finds `extracted/` in the current directory or next to the executable. Otherwise, in **Game data > Folders**, drop the extracted folder or the `.iso` on the window, pick it with *Browse...* / *Disc image...*, or type the path. An `.iso` is extracted to a folder you can change (by default inside the *Files folder* when you set one, else next to the `.iso`).

```bash
# Or extract the game ISO yourself
mkdir -p extracted/
extract-xiso -x asura_wrath.iso -d extracted/

# (Optional) Install DLC Content (Not supported/tested yet)
```

If extracted game is somewhere else, it can also be specified via the `--game_data_root` argument:

```bash
asura_wrath --game_data_root=/path/to/extracted/game
```

Available ReXGlue Path Configuration Flags:
| Flag | Description | Default |
| - | - | - |
| `--cache_root` | Directory for the SDK's shader and pipeline storage. | `~/.local/share/asura_wrath_recomp/cache` |
| `--game_data_root` | Root directory containing extracted game files. | launcher setting, or `extracted` (if exists) |
| `--metadata_root` | Directory for game metadata. | `(empty)` |
| `--update_data_root` | Directory containing game update / DLC patches. | `(empty)` |
| `--user_data_root` | Directory for user save games and profile data. | `~/.local/share/asura_wrath_recomp` |

The mantra shader cache lives in `~/.cache/asura_wrath_recomp/mantra_shaders.mshc`, or in the launcher's *Files folder* (`ASURA_DATA_DIR`) when one is set (`MANTRA_SHADER_CACHE` to override). Build it once from the launcher (**Game data > Shaders**, about 10 minutes on all cores) or from the command line:

```bash
mantra_shadergen --game extracted --out ~/.cache/asura_wrath_recomp/mantra_shaders.mshc
```

A build can be stopped (**Stop the build**, or closing the launcher): it saves its progress every half minute and on stop in `mantra_shaders.mshc.partial`, and the next build continues from there (**Continue the build**; **Start over** drops it). From the command line, creating `<out>.stop` stops it the same way, the next run continues, and `--fresh` starts over; `--parent <pid>` stops it once that process exits.

Some shaders only exist once the game patches them at runtime (e.g. the menu text). mantra keeps those in `~/.cache/asura_wrath_recomp/shader_dumps/` while you play; **Game data > Shaders** then says how many are missing, and **Add the N new shaders** translates only those, keeping the rest of the cache (**Rebuild all** starts over). From the command line: `--dumps ~/.cache/asura_wrath_recomp/shader_dumps` (repeatable), plus `--update` to keep what the `--out` cache already has (only for a cache of the same format version; otherwise it translates everything).

Without a cache, mantra can't draw what uses an untranslated shader (the screen stays black, audio plays). Once a game started with **Play** has closed, the launcher translates the shaders it used but the cache lacks, in the background at low priority (`mantra_shadergen --update --requested-only --background`); the next run uses them. It also does this while the in-game menu (Esc) is open, since the picture behind the menu can take the swap. It doesn't do it during play: reloading the cache and swapping shaders mid-game caused stutters and briefly missing models. Build the full cache before playing.

---

## 5. Run

### Expected Folder Structure Before Running

#### Linux
```
├── asura_wrath          # Executable
├── extracted/           # .iso contents (or set in the launcher)
├── assets/              # launcher / toast art, app icon (assets/ui/icon.png)
├── librexgpu-mantra.so  # mantra renderer
├── librexgpu-xenos.so   # SDK renderer (--gpu_plugin=xenos)
├── librexruntime.so
├── mantra_shadergen     # shader cache builder
└── libdxcompiler.so     # used by mantra_shadergen
```

#### Windows
```
├── asura_wrath.exe      # Executable
├── extracted/           # .iso contents (or set in the launcher)
├── assets/              # launcher / toast art, app icon
├── rexgpu-mantra.dll    # mantra renderer
├── rexgpu-xenos.dll     # SDK renderer (--gpu_plugin=xenos)
├── rexruntime.dll
├── mantra_shadergen.exe # shader cache builder
├── dxcompiler.dll       # used by mantra_shadergen
└── dxil.dll
```

### Save Locations

| OS | Location |
| :--- | :--- |
| Linux | `~/.local/share/asura_wrath_recomp` (saves, achievements, `cache/`, `logs/`, `asura_wrath_recomp.toml`), or the launcher's *Files folder* |
| Windows | `Documents\asura_wrath_recomp` (saves, achievements, `cache\`, `logs\`), or the launcher's *Files folder* |
| Android | `Android/data/org.asurawrath.recompiled/files/asura_wrath_recomp` |

The launcher's own settings, `launcher.cfg`, sit next to the launcher: beside `asura_wrath` (`asura_wrath.exe`), or beside the `.AppImage`. If that folder can't be written (e.g. a system install), they stay in the folder above. An older `launcher.cfg` from the folder above moves next to the launcher the first time.

### Launcher
```bash
asura_wrath
```

The launcher opens first:

- **Game data**
  - *Folders*: the game folder or `.iso`, and the *Files folder* where the cache, logs and traces go (files already made can be moved there in one click).
  - *Shaders*: build the shader cache, or add only the shaders seen while playing.
  - *Logs*: the log files, newest first. *Detailed logs* (off by default) adds debug messages for bug reports; the logs get much larger.
  - *Traces*: *Trace the next frame* (or F3 in game) writes everything mantra does for one frame to `<Files folder>/traces/<date>_<time>_frame<N>.log`, for picture bugs. The newest 30 are kept.
  - *Info* / *Progress*: your save, achievements and the episodes reached (later ones stay hidden).
- **Controls**: a live button test, controller remapping, deadzones, vibration and Turbo buttons, keyboard and mouse, key rebinding.
  - *Setup*: for a controller the game reads wrongly (a trigger moving the camera, a stick doing nothing). Each control (buttons, triggers, D-pad, every stick direction) is a row, named with both the Xbox and PlayStation labels where they differ (LT / L2, LB / L1, L3): *Set* waits up to 10 seconds for you to press it on the controller, and *Start setup* goes through them all in turn. Each answer is saved at once as an SDL mapping for that controller, in `controller_mappings.txt` next to `launcher.cfg`. On Android it's *Controls > Controller setup* (saved in `controller_setup_android.txt`).
- **Achievements**: list or grid, filtered by unlocked / locked.
- **Settings**
  - *System*: language (game, achievements and launcher), achievement pop-ups. *Português (Brasil) - joao13* is a fan translation shipped with the launcher (see Credits).
  - *Display*: GPU, VSync, display mode, monitor, resolution, *Higher resolution* (experimental), how 16:9 fills other screens, and *Effects* (bloom, depth of field, motion and radial blur, shadows, ambient occlusion, light shafts, lens flares, distortion; turning some off helps slow GPUs, applies at the next start).
  - *Enhancements*: FPS counter, frame rate limit (above 30 is experimental), skip the intro logos, hide the cursor, mute in the background.
- **About**: version, this computer's details with a *Copy* button for bug reports.

**Play** starts the game as a separate process and becomes **Stop**. Settings changed meanwhile apply on the next start. In game, **Esc** opens the launcher over the running game: *Resume* applies most changes at once; ones that need a fresh start turn it into *Restart* (progress since the last save is lost).

In game: F3 traces a frame, F4 settings, F7 achievements, `` ` `` console, Esc the launcher (all rebindable under *Controls > Keyboard*).

The launcher's fonts and achievement icons come from your game files, cached in the *Files folder*. Before a game folder is set it uses Noto Serif from `assets/ui` (SIL Open Font License, `assets/ui/OFL.txt`); `NotoSerifJP-Launcher.awf` holds the Japanese characters of the launcher's text (rerun `tools/make_launcher_font.py` after adding Japanese text).

The render resolution is also `MANTRA_RENDER_SCALE` (a factor such as `1.5`, or a height such as `1080`).

### Android (experimental)

Runs on Android 10+ with an arm64 (or x86_64) CPU and Vulkan 1.1. On-screen touch controls show over the game; they hide while a controller (USB or Bluetooth) is connected (*Controls > Touch controls*). A Hide button between Back and Start hides the rest until tapped again (for cutscenes), and *Controls > Touch layout* moves and resizes each one. Snapdragon phones can also load a custom Vulkan driver such as Turnip (*Settings > GPU driver*).

1. Install the APK (`…-android-arm64-v8a.apk` for phones, or the universal one).
2. Copy the extracted game (the folder with `default.xex`) to the phone, then pick it in the launcher (**Game › Choose folder**, which asks for "All files access"). A copy in `Android/data/org.asurawrath.recompiled/files/extracted/` is found without asking. Or extract the `.iso` on the phone with **Game › Extract an .iso…** (into that folder).
3. **Shader cache**: build it on the phone (slow; keep it charging), or build it on a computer (**Game data > Shaders**) and **Import** the `.mshc` (the same file works on every platform).
4. **Play**. In game, Back opens the in-game menu: the launcher's own pages (controls, button remap, controller setup, achievements, settings) with Resume, Restart and Quit. The desktop's in-game menu isn't used on Android.

What to expect: it is slow on mid-range phones (well below 30 fps on a Snapdragon 695 / Adreno 619 class device), and some character voices can drop out in cutscenes when the frame rate is low. Lowering *Render resolution* and turning off some *Effects* (both under *Settings > Display*) helps. Logs and frame traces (under *Game data*) are welcome in bug reports. More details: [android/README.md](android/README.md).

## 6. Custom Run Flags

#### Skip the Launcher
```bash
asura_wrath --launcher=false --game_data_root=extracted
```

#### Custom Renderer
```bash
asura_wrath --gpu_plugin=mantra   # default
asura_wrath --gpu_plugin=xenos    # the SDK's renderer
```

#### Custom Resolution
```bash
asura_wrath --fullscreen=false --window_width=1920 --window_height=1080
```

The game renders 1280x720 (16:9) and is scaled to the window. On wider screens, `--present_letterbox=false` stretches it.

#### Selecting GPU Device

The launcher lists the GPUs. From the command line, list available GPUs and their index numbers on your system:
```bash
vulkaninfo --summary | grep -E 'GPU[0-9]+:|deviceName'
```

Output:
```text
GPU0:
        deviceName         = Intel(R) UHD Graphics (CML GT2)
GPU1:
        deviceName         = NVIDIA GeForce GTX 1650
GPU2:
        deviceName         = llvmpipe (LLVM 20.1.8, 256 bits)
```

Pass `--vulkan_device=<index>` to target a specific GPU (e.g., index `1` for discrete NVIDIA GPU):
```bash
asura_wrath --vulkan_device=1
```

> The SDK's numbering can differ from `vulkaninfo`'s; the launcher's list uses the SDK's order.

#### Logs and Debugging
```bash
asura_wrath --log_file=out/run.log
```

Without `--log_file`, each start writes `logs/asura_wrath_recomp_NNN.log` in the *Files folder* when one is set, else next to the executable (or under the user data folder if that isn't writable), and the game started by Play writes `asura_wrath_recomp_NNN.game.log` beside it, with the same number. With `--log_file=out/run.log`, the game's log is `out/run-game.log`. **Game data > Logs** lists them, newest first.

At the default `info` level the renderer logs a `frame times` line every 5 seconds and a `hitch:` line for slow frames; the other periodic stats (`cpu:`, shaders, pipelines, textures, draw census) need `--log_level=debug` or *Detailed logs*. Frame traces go to `<Files folder>/traces/` (`MANTRA_TRACE_DIR` to override). `MANTRA_*` environment variables (traces, captures, per-frame stats) are documented where they are read in `src/mantra_renderer/`.

---

## 7. Credits

- Powered by **ReXGlue** (https://github.com/rexglue/rexglue-sdk), an open-source static recompilation framework for Xbox 360 software.
- Shader translation by **XenosRecomp** (https://github.com/hedge-dev/XenosRecomp), © hedge-dev and contributors, MIT License (`thirdparty/XenosRecomp/LICENSE.md`): `mantra_shadergen` uses it to turn the game's Xenos shaders into the shader cache.
- Brazilian Portuguese translation (*Settings > System > Language > Português (Brasil) - joao13*) by **XEX Brasil** and **Brazilian Warriors** (Xujozer, Angel Forgotten, Sergio-F), revised by JMC360, published at https://joao13traducoes.com/2021/07/x360-asura-wrath-xex-brasil-e-brazilian-warriors/. It ships in `assets/translations/pt-BR` (`CREDITS.txt` there), with the few lines it left in Spanish or untranslated completed; `tools/make_ptbr_pack.py` rebuilds it from the downloaded patch. Its translated images (title menu, Game Over, end screens) are included; the launcher gives the game a table of contents (`Xbox360TOC.txt`) with their new sizes, as the patch does.
- UI touch controls made possible by using **kenney.nl** Xbox gamepad assets (https://kenney.nl/assets/input-prompts)
- *Asura's Wrath* and its artwork are © **CAPCOM** (https://www.capcom.com). The launcher and pop-up images in `assets/ui` come from the game, with some parts edited by hand for the launcher. The game itself, its fonts and other assets are not included: they are read from your own copy.
- Fallback fonts: **Noto Serif** and **Noto Serif CJK JP** (https://notofonts.github.io), © The Noto Project Authors and Google, under the SIL Open Font License 1.1 (`assets/ui/OFL.txt`). `NotoSerifJP-Launcher.awf` is a subset rendered from Noto Serif CJK JP.

---

## 8. Links

- Another Asura's Wrath PC port, with better performance than this one: https://github.com/omar2300/asuras-wrath-pc/ (help them out with playthrough issues).

---

## 9. Issues

- Episodes are still being tested: Episode 1 seems playable with some minor visual issues, the others are still to be tested.
- Android is experimental: slow on mid-range phones, character voices can drop out at low frame rates.

---

## 10. TODO

- [ ] Port to Android (experimental build available; needs performance work)
- [x] Add touch controls for Android
- [ ] Test all episodes
- [ ] Improve performance
- [ ] Make DLCs work
- [x] Port to Windows (natively or cross-compiled from Linux;)
- [x] Make consistent builds
- [x] Make it easier to build & run
