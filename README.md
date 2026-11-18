# Asura's Wrath Recompiled

PC port of Asura's Wrath (`default.xex`, Title ID `43430817`) via Xbox 360 static PowerPC recompilation to C++23, rendered by the in-repo Vulkan renderer `mantra`.

> Note: Linux only for now (Windows and Android are on the TODO list).

<a href="https://www.youtube.com/playlist?list=PLVyuezqTsTB0">
        <img alt="cover" src="https://github.com/user-attachments/assets/339f7f1b-85d4-48ac-9b5f-ac4fad276dad" />
</a>

https://www.youtube.com/playlist?list=PLVyuezqTsTB0

> Original illustration by Satoshi Sakai

---

## 1. Requirements

- A copy of the Xbox 360 game ISO (`Asura's Wrath.iso`).
- Optional: [extract-xiso](https://github.com/XboxDev/extract-xiso) to extract the `.iso` yourself (the launcher can also extract it).
- clang-20, CMake 3.25+, Ninja, glslc (shaderc), Vulkan drivers, Git.

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

### Package

```bash
# Release archive with just the binaries
tools/package.sh

# Also an AppImage (needs appimagetool)
tools/package.sh --appimage

# out/
# ├── build
# │   └── linux-amd64-release
# └── package
#     ├── asuras-wrath-recomp/
#     ├── asuras-wrath-recomp-<version>-linux-x86_64.tar.gz
#     └── asuras-wrath-recomp-<version>-x86_64.AppImage
```

The default build is tuned for your CPU and may not run on others: build with `./build.sh --portable` first for a package to share. Players supply their own game.

---

## 4. Game Setup

The launcher (first window when starting the game) finds `extracted/` in the current directory or next to the executable. Otherwise, in **System**, drop the extracted folder or the `.iso` on the window (an `.iso` is extracted to a folder of your choice), or type the path.

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

The mantra shader cache lives in `~/.cache/asura_wrath_recomp/mantra_shaders.mshc` (`MANTRA_SHADER_CACHE` to override). Build it once from the launcher (**Graphics > Shaders**, about 10 minutes on all cores) or from the command line:

```bash
mantra_shadergen --game extracted --out ~/.cache/asura_wrath_recomp/mantra_shaders.mshc
```

Some shaders only exist once the game patches them at runtime (e.g. the menu text). mantra keeps those in `~/.cache/asura_wrath_recomp/shader_dumps/` while you play; **Graphics > Shaders** then says how many are missing, and **Rebuild** adds them (from the command line: `--dumps ~/.cache/asura_wrath_recomp/shader_dumps`, repeatable).

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

### Save Locations

| OS | Location |
| :--- | :--- |
| Linux | `~/.local/share/asura_wrath_recomp` (saves, achievements, `launcher.cfg`, `asura_wrath_recomp.toml`) |

### Linux
```bash
asura_wrath
```

The launcher opens first, in a window:

- **Controls**: *Controller* tab (live test of buttons, sticks and triggers, button remapping, stick deadzones, 0% by default, vibration strength) and *Keyboard* tab (play with the keyboard, mouse camera, key rebinding, and the overlay keys F3/F4/F7/`).
- **Achievements**: the list (read from your `default.xex`) and your unlocks, newest first, as a list or a grid, filtered by all / unlocked / locked.
- **Saves**: your save (last saved, size, folder and its files, difficulty, battle points) and its story progress: the episodes you have reached, by Part, with their best rank on Easy/Normal/Hard (titles from your game files, in your language; later episodes stay hidden to avoid spoilers). Open its folder or delete it.
- **Graphics**, in three tabs:
  - *Display*: GPU (Automatic shows which one it picks), VSync, display mode (windowed, borderless or exclusive fullscreen), monitor, resolution (from the monitor's modes), *Higher resolution* (experimental, off by default: the game renders at its own 1280x720, scaled to the window) and the render resolution it uses (960x540 to 3840x2160; some effects can still show green areas), and how the 16:9 picture fills wide or 4:3 screens (fit / fill / stretch).
  - *Shaders*: the shader cache (build progress, its folder).
  - *Enhancements*: an FPS counter; a frame rate limit slider (30, the game's, to 240 in steps of 5, or unlimited, with 30/60/120/144 buttons; above 30 is experimental: `src/game/frame_rate.cpp` replaces the engine's `GetMaxTickRate`); Turbo (hold a ticked controller button A/B/X/Y/LB/RB instead of mashing it; it repeats about 12 times a second); hiding the mouse cursor in game while it isn't moving; muting while the window isn't focused (the SDK's `audio_mute`).

  A note above Play says when the shader cache is missing, or too old for the chosen render resolution (caches built before render scaling only work at 1280x720); it goes away if you change the setting back, and clicking it opens *Shaders*. Pressing Play then opens *Shaders* first; a second press starts anyway.
- **System**: game folder (or `.iso`), language (English, Japanese, German, French, Spanish, Italian) for the game, the achievements and the launcher itself; achievement pop-ups (on/off, corner, duration).

Controls, the Graphics tabs and System each have a *Reset to defaults* button.

The launcher's fonts (including the accented and Japanese ones from the game's localized packages) and achievement icons are read from your game folder into `~/.cache/asura_wrath_recomp/`. Until then (no game folder yet) the launcher uses its own fonts from `assets/ui`: Noto Serif Regular/Bold and `NotoSerifJP-Launcher.awf`, the Japanese characters of the launcher's text rendered from Noto Serif CJK JP by `tools/make_launcher_font.py` (rerun it after adding Japanese text). They are under the SIL Open Font License (`assets/ui/OFL.txt`).

**Play** starts the game in its own window, as a separate process; the launcher stays open and the button becomes **Stop** (closes the game; progress since the last save is lost). Settings changed in the launcher meanwhile apply the next time the game starts. Building shaders and extracting an ISO wait until the game is closed. When the game exits, the launcher picks up settings changed in game and rescans saves and achievements.

In game, **Esc** opens the launcher over the running game (rebind it under *Controls > Keyboard*). The game keeps running underneath. *Resume* (or Esc again) goes back and applies keys, controller, pop-up, window size and Enhancements changes at once. Changes that need a fresh start (graphics card, VSync, monitor, exclusive fullscreen, Fill, render resolution, game folder, language) turn the button into *Restart*, which restarts the game: progress since the last save is lost. Esc instead keeps them for the next start.

The render resolution is also `MANTRA_RENDER_SCALE` (a factor such as `1.5`, or a height such as `1080`). Above 1x, rebuild the shader cache once (cache v8) so screen-space effects line up.

In game: F3 performance overlay, F4 settings, F7 achievements, `` ` `` console.

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

With the launcher, the game started by Play logs to `out/run-game.log` (the launcher keeps `out/run.log`).

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

- [ ] Port to Windows
- [ ] Port to Android
- [ ] Add touch controls for Android (?)
- [ ] Test all episodes
- [ ] Improve performance
- [ ] Make DLCs work
- [x] Make consistent builds
- [x] Make it easier to build & run
