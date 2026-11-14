#pragma once
// XenosRecomp wrapper: Xbox 360 shader container -> HLSL -> SPIR-V (DXC).
// Kept in its own translation unit: XenosRecomp's headers define global
// helpers (be<>, byteSwap) that would clash with the rexglue headers.

#include <cstdint>
#include <string>
#include <vector>

namespace mantra_shadergen {

struct TranslateResult {
  bool ok = false;
  // When not ok: what failed.
  enum class Failure { kNone, kStageInterface, kDxc } failure = Failure::kNone;
  bool is_pixel = false;
  uint32_t spec_constants_mask = 0;
  // Xenos fetch constant slots the shader samples through (bit per slot).
  uint32_t texture_slot_mask = 0;
  // Bool constant registers the shader tests (bit per register 0..15);
  // bool_overflow: it tests a register outside the synthetic b0..b15.
  uint32_t bool_mask = 0;
  bool bool_overflow = false;
  // Pixel shaders: XenosRecomp's PixelShaderOutputs (bits 0..3 = oC0..oC3,
  // bit 4 = oDepth).
  uint32_t ps_outputs = 0;
  // Vertex shaders: one packed mantra::shader_cache::VertexElement per vertex
  // input (ucode address of its vfetch + Vulkan input location).
  std::vector<uint32_t> vertex_elements;
  std::string hlsl;
  std::vector<uint8_t> spirv;
};

// May crash on shaders XenosRecomp does not support (it asserts instead of
// reporting errors); callers run it in a child process.
TranslateResult TranslateContainer(const uint8_t* container);

// Replaces a container's microcode with `microcode` of the same size (a
// runtime-patched variant of the same shader). False if the sizes differ.
bool ReplaceMicrocode(std::vector<uint8_t>& container, const std::vector<uint8_t>& microcode);

}  // namespace mantra_shadergen
