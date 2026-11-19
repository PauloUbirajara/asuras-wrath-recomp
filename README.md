# Asura's Wrath Recompiled

PC port of Asura's Wrath (`default.xex`, Title ID `43430817`) via Xbox 360 static PowerPC recompilation to C++23, rendered by the in-repo Vulkan renderer `mantra`.

> Note: made and tested on Linux. A Windows build can be made (natively or cross-compiled from Linux) but hasn't been tested on Windows yet. Android is on the TODO list.

<a href="https://www.youtube.com/playlist?list=PLVyuezqTsTB0">
        <img alt="cover" src="https://github.com/user-attachments/assets/339f7f1b-85d4-48ac-9b5f-ac4fad276dad" />
</a>

https://www.youtube.com/playlist?list=PLVyuezqTsTB0

> Original illustration by Satoshi Sakai

---

## 1. Requirements

- A copy of the Xbox 360 game ISO (`Asura's Wrath.iso`).
- Optional: [extract-xiso](https://github.com/XboxDev/extract-xiso) to extract the `.iso` yourself (the launcher can also extract it).
- Linux: clang-20, CMake 3.25+, Ninja, glslc (shaderc), Vulkan drivers, Git.
- Windows (native build): Visual Studio 2022 Build Tools (Windows SDK + C runtime), LLVM clang 20+ on PATH, CMake 3.25+, Ninja, Git, the Vulkan SDK.
- Windows (cross build from Linux): the Linux requirements plus lld-20 and llvm-20 (`llvm-rc`, `llvm-ar`, `llvm-mt`); `tools/setup_windows_cross.sh` downloads Microsoft's SDK and runtime (see below).

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

### Package

```bash
# A release: portable build, then the archive + AppImage named after the version
./build.sh --portable
tools/package.sh --appimage --clean --version 0.1.0

# out/package/
# ├── asuras-wrath-recomp-0.1.0-linux-x86_64.tar.gz
# ├── asuras-wrath-recomp-0.1.0-linux-x86_64.AppImage
# └── SHA256SUMS
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

Some shaders only exist once the game patches them at runtime (e.g. the menu text). mantra keeps those in `~/.cache/asura_wrath_recomp/shader_dumps/` while you play; **Game data > Shaders** then says how many are missing, and **Add the N new shaders** translates only those, keeping the rest of the cache (**Rebuild all** starts over). From the command line: `--dumps ~/.cache/asura_wrath_recomp/shader_dumps` (repeatable), plus `--update` to keep what the `--out` cache already has (only for a cache of the same format version; otherwise it translates everything).

Without a cache, mantra can't draw what uses an untranslated shader (the screen stays black, audio plays). When the game is started with **Play**, the launcher translates the shaders the game has used but the cache lacks in the background, at low priority (`mantra_shadergen --update --requested-only --background`), and the running game picks up the new cache within a few seconds. The first run has to read the game files once (cached in `shader_scan.bin` next to the shader cache, so later runs skip it), so building the full cache before playing is still the smoothest.

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
| Linux | `~/.local/share/asura_wrath_recomp` (saves, achievements, `launcher.cfg`, `asura_wrath_recomp.toml`) |
| Windows | `Documents\asura_wrath_recomp` (saves, achievements, `cache\`, `logs\`), or the launcher's *Files folder* |

### Linux
```bash
asura_wrath
```

The launcher opens first, in a window:

- **Game data**, in tabs:
  - *Folders*: game folder (or `.iso`; type it, pick it with *Browse...* / *Disc image...*, or drop it on the window); *Files folder*, where the shader cache, logs, UI art and other generated files go (default `~/.cache/asura_wrath_recomp/`; pick one, or drop a folder that isn't the game on the window; files already made can be moved there in one click, and logs follow on the next start).
  - *Shaders*: the shader cache (build progress, its folder; adding only the shaders seen while playing that it doesn't have yet).
  - *Logs*: the log files, newest first, with their folder (see [Logs and Debugging](#logs-and-debugging)).
  - *Traces*: *Trace the next frame* (while the game runs) makes mantra write everything it does for one frame into the game's log (`trace:` lines), for picture bugs. When the frame is done the game shows a short "Frame N traced" toast (mantra writes N to `<trigger>.done`), and the button stays disabled until then. The table lists the traces found in the logs. It works through `MANTRA_TRACE_TRIGGER`, which the game sets to `<Files folder>/trace.now` unless the shell already set it. Buffer and texture dumps also need `MANTRA_CAPTURE_DIR`.
  - *Info*: last saved, size, profile, achievements unlocked, folder and its files; open the folder or delete the save.
  - *Progress*: difficulty, battle points and the episodes you have reached, by Part, with their best rank on Easy/Normal/Hard; titles from your game files, in your language; later episodes stay hidden to avoid spoilers.
- **Controls**: *Test* tab (live view of the buttons as the game sees them, sticks with their deadzones, and triggers), *Controller* tab (button remapping, stick deadzones, 0% by default, vibration strength with a Test button) and *Keyboard* tab (play with the keyboard, mouse camera, key rebinding, and the overlay keys F3/F4/F7/`).
- **Achievements**: the list (read from your `default.xex`) and your unlocks, newest first, as a list or a grid, filtered by all / unlocked / locked.
- **Settings**, in tabs:
  - *System*: language (English, Japanese, German, French, Spanish, Italian) for the game, the achievements and the launcher itself; achievement pop-ups (on/off, corner, duration).
  - *Display*: GPU (Automatic shows which one it picks), VSync, display mode (windowed, borderless or exclusive fullscreen), monitor, resolution (from the monitor's modes), *Higher resolution* (experimental, off by default: the game renders at its own 1280x720, scaled to the window) and the render resolution it uses (960x540 to 3840x2160; some effects can still show green areas), and how the 16:9 picture fills wide or 4:3 screens (fit / fill / stretch).
  - *Enhancements*: an FPS counter (off by default, in a corner of your choice); a frame rate limit slider (30, the game's, to 240 in steps of 5, or unlimited, with 30/60/120/144 buttons; above 30 is experimental: `src/game/frame_rate.cpp` replaces the engine's `GetMaxTickRate`, and since the game presents every second display refresh, the launcher also sets `video_mode_refresh_rate` to twice the cap, up to 240; that part applies when the game starts); skipping the intro (the Capcom, CyberConnect2, Unreal Engine, SpeedTree and Bink logos and the autosave notice; `src/game/skip_intro.cpp` mounts a copy of the Movies folder in `~/.cache/asura_wrath_recomp/movies` where those movies are cut to their first, black, frame and the rest are links to the originals); Turbo (hold a ticked controller button A/B/X/Y/LB/RB instead of mashing it; it repeats about 12 times a second); hiding the mouse cursor in game while it isn't moving; muting while the window isn't focused (the SDK's `audio_mute`).

  A note above Play says when the shader cache is missing, or too old for the chosen render resolution (caches built before render scaling only work at 1280x720); it goes away if you change the setting back, and clicking it opens *Game data > Shaders*. Pressing Play then opens *Shaders* first; a second press starts anyway.
- **About**: the repository link, the version (git commit, branch, build date), this computer (OS, kernel, desktop, CPU, memory, GPUs with their driver and Vulkan versions) with a *Copy* button for bug reports, and a notice that this is an unofficial fan project, not affiliated with Capcom or CyberConnect2. The version comes from `build_info.h`, which `tools/build_info.cmake` regenerates on each build from git.

Controls and the Settings tabs each have a *Reset to defaults* button.

The launcher's fonts (including the accented and Japanese ones from the game's localized packages) and achievement icons are read from your game folder into `~/.cache/asura_wrath_recomp/`. Until then (no game folder yet) the launcher uses its own fonts from `assets/ui`: Noto Serif Regular/Bold and `NotoSerifJP-Launcher.awf`, the Japanese characters of the launcher's text rendered from Noto Serif CJK JP by `tools/make_launcher_font.py` (rerun it after adding Japanese text). They are under the SIL Open Font License (`assets/ui/OFL.txt`).

**Play** starts the game in its own window, as a separate process; the launcher stays open and the button becomes **Stop** (closes the game; progress since the last save is lost). Settings changed in the launcher meanwhile apply the next time the game starts. Building shaders and extracting an ISO wait until the game is closed. When the game exits, the launcher picks up settings changed in game and rescans saves and achievements.

In game, **Esc** opens the launcher over the running game (rebind it under *Controls > Keyboard*). The game keeps running underneath. There the launcher is drawn translucent black with light text instead of its background art, so the game shows through. *Resume* (or Esc again) goes back and applies keys, controller, pop-up, window size and Enhancements changes at once. Changes that need a fresh start (graphics card, VSync, monitor, exclusive fullscreen, Fill, render resolution, game folder, language) turn the button into *Restart*, which restarts the game: progress since the last save is lost. Esc instead keeps them for the next start.

The render resolution is also `MANTRA_RENDER_SCALE` (a factor such as `1.5`, or a height such as `1080`). Above 1x, rebuild the shader cache once (cache v8) so screen-space effects line up.

In game: F3 traces the next frame (*Game data > Traces*), F4 settings, F7 achievements, `` ` `` console, Esc the launcher. All are rebindable under *Controls > Keyboard* (right-click a key to clear it). The SDK's performance overlay has no key by default; give it one there if you want it.

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

Without `--log_file`, each start writes `logs/asura_wrath_recomp_NNN.log` in the *Files folder* when one is set, else next to the executable (or under the user data folder if that isn't writable), and the game started by Play writes `asura_wrath_recomp_NNN.game.log` beside it, with the same number. With `--log_file=out/run.log`, the game's log is `out/run-game.log`. **Settings > Logs** lists them, newest first.

Performance lines (`frame times`, `cpu:`, `hitch:`) are logged every 5 seconds. `MANTRA_*` environment variables (traces, captures, per-frame stats) are documented where they are read in `src/mantra_renderer/`.

---

## 7. Credits

- Powered by **ReXGlue** (https://github.com/rexglue/rexglue-sdk), an open-source static recompilation framework for Xbox 360 software.
- UI touch controls made possible by using **kenney.nl** Xbox gamepad assets (https://kenney.nl/assets/input-prompts)
- *Asura's Wrath* and its artwork are © **CAPCOM** (https://www.capcom.com). The launcher and pop-up images in `assets/ui` come from the game, with some parts edited by hand for the launcher. The game itself, its fonts and other assets are not included: they are read from your own copy.
- Fallback fonts: **Noto Serif** and **Noto Serif CJK JP** (https://notofonts.github.io), © The Noto Project Authors and Google, under the SIL Open Font License 1.1 (`assets/ui/OFL.txt`). `NotoSerifJP-Launcher.awf` is a subset rendered from Noto Serif CJK JP.

---

## 8. Links

- Another Asura's Wrath PC port, with better performance than this one: https://github.com/omar2300/asuras-wrath-pc/ (help them out with playthrough issues).

---

## 9. Issues

- Episodes are still being tested: Episode 1 seems playable with some minor visual issues, the others are still to be tested.

---

## 10. TODO

- [ ] Port to Windows (builds natively and cross-compiled from Linux; not tested on Windows yet)
- [ ] Port to Android
- [ ] Add touch controls for Android (?)
- [ ] Test all episodes
- [ ] Improve performance
- [ ] Make DLCs work
- [x] Make consistent builds
- [x] Make it easier to build & run
