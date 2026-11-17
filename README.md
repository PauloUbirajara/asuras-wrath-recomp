# Asura's Wrath Recompiled

Static recompilation of Asura's Wrath (Xbox 360) on the rexglue SDK, with the
in-repo Vulkan renderer `mantra`. Linux only. You need your own copy of the game.

## Requirements

clang-20, CMake ≥ 3.25, Ninja, glslc (shaderc), Vulkan drivers, Git.

## Clone

```bash
git clone --recursive https://github.com/PauloUbirajara/asuras-wrath-recomp.git
cd asuras-wrath-recomp
git -C thirdparty/XenosRecomp submodule update --init --recursive
```

## Game files

Extract your disc image into `extracted/` (it must contain `default.xex` and `BCGame/`):

```bash
extract-xiso -x asura_wrath.iso -d extracted
```

## Build

```bash
./build.sh
```

`./build.sh --help` lists the options (`--debug`, `--portable`, `--clean`).
Outputs go to `out/build/linux-amd64-release/`.

## Shader cache

Translate the game's shaders once (takes about 10 minutes and uses every core).
Each player generates their own cache, because it is built from the game files:

```bash
out/build/linux-amd64-release/mantra_shadergen --game extracted --out ~/.cache/asura_wrath/mantra_shaders.mshc
```

`MANTRA_SHADER_CACHE=<file>` points the game at a different cache. Optionally,
play once with `MANTRA_SHADER_DUMP=out/mantra_dumps` and add `--dumps out/mantra_dumps`
when regenerating, to also cache the shader variants the game patches at runtime.

## Run

```bash
out/build/linux-amd64-release/asura_wrath --game_data_root=extracted
```

Useful flags: `--fullscreen=false`, `--vulkan_device=N` (GPU index),
`--log_file=out/run.log`.

## Layout

| Path | Contents |
|---|---|
| `src/` | game app, custom UI (`src/ui`), mantra renderer (`src/mantra_renderer`) |
| `tools/` | `mantra_shadergen`, `xxx_unpack`, `asset_extract.py` |
| `assets/` | project-made art (UI, launcher); copied next to the executable on build |
| `generated/` | recompiled game code |
| `thirdparty/` | rexglue SDK, XenosRecomp |
| `out/` | build outputs and local game-derived files (not in git) |
