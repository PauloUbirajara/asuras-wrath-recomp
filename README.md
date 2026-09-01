# Asura's Wrath Recompiled

PC port of Asura's Wrath (`default.xex`, Title ID `43430817`) via Xbox 360 static PowerPC recompilation to C++23.

<a href="https://www.youtube.com/playlist?list=PLVyuezqTsTB0">
        <img width="1280" height="640" alt="cover" src="https://github.com/user-attachments/assets/339f7f1b-85d4-48ac-9b5f-ac4fad276dad" />
</a>

https://www.youtube.com/playlist?list=PLVyuezqTsTB0

> Original illustration by Satoshi Sakai

---

## 1. Requirements

- A copy of the Xbox 360 game ISO (`Asura's Wrath.iso`).
- `extract-xiso` tool to extract .iso game contents into `extracted/` (https://github.com/XboxDev/extract-xiso).
- `ReXGlue SDK` (https://github.com/rexglue/rexglue-sdk)

## 2. Environment Variables

- `REXSDK_DIR`: Path to the ReXGlue SDK directory.

---

## 3. Build & Run

```bash
# Clone repo
git clone https://github.com/PauloUbirajara/asuras-wrath-recomp.git
cd asuras-wrath-recomp


# Extract ISO
mkdir -p extracted/
extract-xiso -x asura_wrath.iso -d extracted/


# (Optional) Copy DLC folder to extracted content directory
# No need to re-build if added after building
mkdir -p extracted/content/0000000000000000/

# cp -r <path_to_DLC_folder> extracted/content/0000000000000000/
cp -r 43430817 extracted/content/0000000000000000/


# Build
# export REXSDK_DIR=/path/to/rexglue-sdk
# If error, maybe try removing the build/ folder inside rexglue-sdk and try again

./build.sh


# Run
./build/asura_wrath_recomp
```

### Combined Example (what currently works best for me)
```bash
./build/asura_wrath_recomp \
    --vulkan_device=1 \
    --render_target_path_vulkan=fsi \
    --window_width=1920 \
    --window_height=1080 \
    --vulkan_pipeline_creation_threads=6 \
    --async_shader_compilation=true \
    --vulkan_async_skip_incomplete_frames=true
```

### Example: Custom Resolution
```bash
./build/asura_wrath_recomp --window_width=1920 --window_height=1080
```

### Example: Selecting GPU Device

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
./build/asura_wrath_recomp --vulkan_device=1
```

### Example: Render Target Path (`fsi` vs `fbo`)

- `fbo`: Host framebuffers (default, higher performance, can produce color artifacts on some GPUs).
- `fsi`: Fragment Shader Interlock (software EDRAM pixel packing, fixes red overlay artifacts on NVIDIA GPUs).

Fix red overlay artifact on NVIDIA GPUs:
```bash
./build/asura_wrath_recomp --vulkan_device=1 --render_target_path_vulkan=fsi
```

Switch back to host framebuffers:
```bash
./build/asura_wrath_recomp --render_target_path_vulkan=fbo
```

---

## 4. Credits

- Powered by **ReXGlue**, an open-source static recompilation framework for Xbox 360 software.

---

## 5. Issues

- Had to use FSI over FBO to avoid red artifacts (performance loss / stutters due to on-demand texture / shader compilation - also seen in https://github.com/rexglue/rexglue-sdk/blob/71782a3bc15cd1994381757fae7d616242f22e6a/src/graphics/vulkan/render_target_cache.cpp#L44-L63)

---

## 6. TODO

- [ ] Test DLCs
- [ ] Test for Windows (?)
- [ ] Make it easier to build & run
