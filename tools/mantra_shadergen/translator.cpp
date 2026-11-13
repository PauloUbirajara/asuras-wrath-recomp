#include "translator.h"

#include <regex>
#include <string>
#include <unordered_map>

// XenosRecomp (thirdparty/XenosRecomp, unmodified). pch.h first: the other
// headers rely on it.
#include "pch.h"
#include "dxc_compiler.h"
#include "shader_recompiler.h"

#include "shader_common_hlsl.h"  // generated: kShaderCommonHlsl (mantra_shader_common.hlsl)

namespace mantra_shadergen {
namespace {

// Adapts XenosRecomp's SPIR-V interface (buffer device addresses, runtime
// descriptor indices) to mantra_shader_common.hlsl; see the comment there.
void AdaptInterface(TranslateResult& r) {
  std::string& hlsl = r.hlsl;

  // Descriptor indices -> the sampler register (= fetch constant slot).
  // XenosRecomp places them at SharedConstants + dim * 64 + register * 4
  // (dim 0..2 = 2D/3D/Cube, 3 = sampler).
  static const std::regex kIndexDefine(
      R"(#define (\w+)_(Texture2D|Texture3D|TextureCube|Sampler)DescriptorIndex vk::RawBufferLoad<uint>\(g_PushConstants\.SharedConstants \+ (\d+)\))");
  std::string out;
  out.reserve(hlsl.size());
  auto begin = hlsl.cbegin();
  for (std::sregex_iterator it(hlsl.begin(), hlsl.end(), kIndexDefine), end; it != end; ++it) {
    const std::smatch& m = *it;
    const std::string kind = m[2];
    const uint32_t dim = kind == "Texture2D" ? 0 : kind == "Texture3D" ? 1 : kind == "TextureCube" ? 2 : 3;
    const uint32_t offset = uint32_t(std::stoul(m[3]));
    const uint32_t slot = (offset - dim * 64) / 4;
    out.append(begin, m[0].first);
    out += "#define " + std::string(m[1]) + "_" + kind + "DescriptorIndex " + std::to_string(slot);
    begin = m[0].second;
  }
  out.append(begin, hlsl.cend());
  hlsl = std::move(out);

  // vk::RawBufferLoad<T>( -> MantraLoad_T(
  static const std::regex kRawLoad(R"(vk::RawBufferLoad<(\w+)>\()");
  hlsl = std::regex_replace(hlsl, kRawLoad, "MantraLoad_$1(");

  // Usage masks. mantra_shadergen declares every sampler (s0..s15) and bool
  // (b0..b15) in its synthetic constant tables, so look at the uses, not at
  // the declarations: fetches pass "<name>_*DescriptorIndex" as call arguments, and
  // branches test "g_Booleans & <name>" (or "b<N>" when undeclared).
  static const std::regex kSamplerSlot(R"(#define (\w+)_SamplerDescriptorIndex (\d+))");
  static const std::regex kSamplerUse(
      R"([(,] ?(\w+)_(?:Texture2D|Texture3D|TextureCube|Sampler)DescriptorIndex\b)");
  static const std::regex kBoolDefine(R"(\t#define (\w+) \(1 << (\d+)\))");
  static const std::regex kBoolUse(R"(g_Booleans & (\w+)\))");
  static const std::regex kUndeclaredBool(R"(if \(b\d+ [!=]= 0\))");
  std::unordered_map<std::string, uint32_t> sampler_slots;
  for (std::sregex_iterator it(hlsl.begin(), hlsl.end(),
                               kSamplerSlot), end;
       it != end; ++it) {
    sampler_slots[(*it)[1]] = uint32_t(std::stoul((*it)[2]));
  }
  r.texture_slot_mask = 0;
  for (std::sregex_iterator it(hlsl.begin(), hlsl.end(),
                               kSamplerUse), end;
       it != end; ++it) {
    auto slot = sampler_slots.find((*it)[1]);
    if (slot != sampler_slots.end() && slot->second < 32) {
      r.texture_slot_mask |= 1u << slot->second;
    }
  }
  std::unordered_map<std::string, uint32_t> bool_bits;
  for (std::sregex_iterator it(hlsl.begin(), hlsl.end(),
                               kBoolDefine), end;
       it != end; ++it) {
    bool_bits[(*it)[1]] = uint32_t(std::stoul((*it)[2]));
  }
  for (std::sregex_iterator it(hlsl.begin(), hlsl.end(), kBoolUse), end;
       it != end; ++it) {
    auto bit = bool_bits.find((*it)[1]);
    if (bit != bool_bits.end()) {
      const uint32_t reg = bit->second - (r.is_pixel ? 16 : 0);
      if (reg < 32) {
        r.bool_mask |= 1u << reg;
      }
    }
  }
  r.bool_overflow = std::regex_search(hlsl, kUndeclaredBool);
}

}  // namespace

TranslateResult TranslateContainer(const uint8_t* container) {
  TranslateResult result;
  ShaderRecompiler recompiler;
  recompiler.recompile(container, kShaderCommonHlsl);
  result.is_pixel = recompiler.isPixelShader;
  result.spec_constants_mask = recompiler.specConstantsMask;
  result.hlsl = std::move(recompiler.out);
  AdaptInterface(result);

  static DxcCompiler dxc;
  IDxcBlob* spirv = dxc.compile(result.hlsl, result.is_pixel, false, true);
  if (!spirv) {
    return result;
  }
  const auto* bytes = static_cast<const uint8_t*>(spirv->GetBufferPointer());
  result.spirv.assign(bytes, bytes + spirv->GetBufferSize());
  spirv->Release();
  result.ok = true;
  return result;
}

}  // namespace mantra_shadergen
