# Asura's Wrath Recompiled

# Update 2026/11/07
Still working slowly on free time with xenon/xenosrecomp tools, but wanted to link another repo that managed to achieve superior performance than this one for PC (help him out with playthrough issues)

https://github.com/omar2300/asuras-wrath-pc/

---

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

# Build for any x86-64 CPU with SSE4.1 (the default uses -march=native)
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

Build with `./build.sh --portable` first for a package that runs on other CPUs. Players supply their own game.

---

## 4. Game Setup

The launcher (first window when starting the game) finds `extracted/` in the current directory or next to the executable. Otherwise, in **Settings**, drop the extracted folder or the `.iso` on the window (an `.iso` is extracted to a folder of your choice), or type the path.

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

The mantra shader cache lives in `~/.cache/asura_wrath_recomp/mantra_shaders.mshc` (`MANTRA_SHADER_CACHE` to override). Build it once from the launcher (**Settings > Shader cache**, about 10 minutes on all cores) or from the command line:

```bash
mantra_shadergen --game extracted --out ~/.cache/asura_wrath_recomp/mantra_shaders.mshc
```

---

## 5. Run

### Expected Folder Structure Before Running

#### Linux
```
├── asura_wrath          # Executable
├── extracted/           # .iso contents (or set in the launcher)
├── assets/              # launcher / toast art
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

- **Settings**: game folder, GPU (Automatic shows which one it picks), VSync, fullscreen, window size, how the 16:9 picture fills wide screens (fit / fill / stretch), shader cache.
- **Controls**: play with the keyboard, mouse camera, key rebinding.
- **Achievements**: the list and your unlocks (after the first start).

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

Performance lines (`frame times`, `cpu:`, `hitch:`) are logged every 5 seconds. `MANTRA_*` environment variables (traces, captures, per-frame stats) are documented where they are read in `src/mantra_renderer/`.

---

## 7. Credits

- Powered by **ReXGlue** (https://github.com/rexglue/rexglue-sdk), an open-source static recompilation framework for Xbox 360 software.
- UI touch controls made possible by using **kenney.nl** Xbox gamepad assets (https://kenney.nl/assets/input-prompts)

---

## 8. Issues

- Episodes are still being tested: Episode 1 seems playable with some minor visual issues, the others are still to be tested.

---

## 9. TODO

- [ ] Port to Windows
- [ ] Port to Android
- [ ] Add touch controls for Android (?)
- [ ] Test all episodes
- [ ] Improve performance
- [ ] Make DLCs work
- [x] Make consistent builds
- [x] Make it easier to build & run
