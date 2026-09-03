# Asura's Wrath Recompiled

PC port of Asura's Wrath (`default.xex`, Title ID `43430817`) via Xbox 360 static PowerPC recompilation to C++23.

<a href="https://www.youtube.com/playlist?list=PLVyuezqTsTB0">
        <img alt="cover" src="https://github.com/user-attachments/assets/339f7f1b-85d4-48ac-9b5f-ac4fad276dad" />
</a>

https://www.youtube.com/playlist?list=PLVyuezqTsTB0

> Original illustration by Satoshi Sakai

---

## 1. Requirements

- A copy of the Xbox 360 game ISO (`Asura's Wrath.iso`).
- [extract-xiso](https://github.com/XboxDev/extract-xiso) tool to extract `.iso` contents.
- CMake 3.25+ and Clang / Ninja compiler toolchain.

---

## 2. Setup

### Clone Repository
```bash
git clone --recursive https://github.com/PauloUbirajara/asuras-wrath-recomp.git
cd asuras-wrath-recomp
```

---

## 3. Build

### Option A: Using CMake Presets (Linux & Windows)

#### Linux
```bash
cmake --preset linux-amd64-release
cmake --build out/build/linux-amd64-release --parallel
```

#### Windows
```cmd
cmake --preset win-amd64-release
cmake --build out/build/win-amd64-release --parallel
```

---

### Option B: Using Build Scripts

#### Linux
```bash
# Build binary
./build.sh

# Build binary and create release zip archive
./build.sh --package
```

#### Windows
```cmd
build.bat
```

---

## 4. Game Setup

In the same directory as the executable, run the following:

```bash
# Extract Game ISO
mkdir -p extracted/
extract-xiso -x asura_wrath.iso -d extracted/

# (Optional) Install DLC Content (Not supported yet)
# Copy DLC title content into the extracted content directory:
# mkdir -p extracted/content/0000000000000000/
# cp -r 43430817 extracted/content/0000000000000000/
```

If extracted game is somewhere else, it can be specified via the `--game_data_root` argument:

```bash
./out/build/linux-amd64-release/asura_wrath_recomp --game_data_root=/path/to/extracted/game
```

Available ReXGlue Path Configuration Flags:
| Flag | Description | Default |
| - | - | - |
| `--game_data_root` | Root directory containing extracted game files. | `extracted` (if exists) or current directory |
| `--user_data_root` | Directory for user save games and profile data. | `~/.local/share/asura_wrath_recomp` (Linux) / `%APPDATA%\asura_wrath_recomp` (Windows) |
| `--cache_root` | Directory for compiled Vulkan pipeline and shader storage. | `cache` |
| `--update_data_root` | Directory containing game update / DLC patches. | `(empty)` |
| `--metadata_root` | Directory for game metadata. | `(empty)` |

---

## 5. Run

#### Linux
```bash
./out/build/linux-amd64-release/asura_wrath_recomp

# Linux (What currently works best for me)
./out/build/linux-amd64-release/asura_wrath_recomp \
    --async_shader_compilation=true \
    --render_target_path_vulkan=fsi \
    --vulkan_async_skip_incomplete_frames=true \
    --vulkan_device=1 \
    --vulkan_pipeline_creation_threads=6 \
    --window_height=1080 \
    --window_width=1920
```

#### Windows
```cmd
out\build\win-amd64-release\asura_wrath_recomp.exe
```

#### Custom Resolution
```bash
./out/build/linux-amd64-release/asura_wrath_recomp --window_width=1920 --window_height=1080
```

#### Selecting GPU Device

List available GPUs and their index numbers on your system:
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
./out/build/linux-amd64-release/asura_wrath_recomp --vulkan_device=1
```

#### Render Target Path (`fsi` vs `fbo`)

- `fbo`: Host framebuffers (default, higher performance, can produce color artifacts on some GPUs).
- `fsi`: Fragment Shader Interlock (software EDRAM pixel packing, fixes red overlay artifacts on NVIDIA GPUs).

Fix red overlay artifact on NVIDIA GPUs:
```bash
./out/build/linux-amd64-release/asura_wrath_recomp --vulkan_device=1 --render_target_path_vulkan=fsi
```

Switch back to host framebuffers:
```bash
./out/build/linux-amd64-release/asura_wrath_recomp --render_target_path_vulkan=fbo
```

---

## 6. Credits

- Powered by **ReXGlue** (https://github.com/rexglue/rexglue-sdk), an open-source static recompilation framework for Xbox 360 software.

---

## 7. Issues

- Had to use FSI over FBO to avoid red artifacts (performance loss / stutters due to on-demand texture / shader compilation - also seen in https://github.com/rexglue/rexglue-sdk/blob/71782a3bc15cd1994381757fae7d616242f22e6a/src/graphics/vulkan/render_target_cache.cpp#L44-L63)

---

## 8. TODO

- [ ] Improve performance
- [ ] Make DLCs work
- [ ] Test for Windows (?)
- [x] Make consistent builds
- [x] Make it easier to build & run