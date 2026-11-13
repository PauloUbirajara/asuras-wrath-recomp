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
#include "mantra_renderer/shader_cache_format.h"

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

  // Host viewport: XenosRecomp ends the VS with the half-pixel offset; mantra
  // applies the full NDC transform of draw_util::GetHostViewportInfo there
  // (viewport cropping, window offset, half-pixel offset, Z range).
  if (!r.is_pixel) {
    static const std::regex kHalfPixel(R"(oPos\.xy \+= g_HalfPixelOffset \* oPos\.w;)");
    hlsl = std::regex_replace(hlsl, kHalfPixel,
                              "oPos.xyz = oPos.xyz * g_NdcScale + g_NdcOffset * oPos.w;");
  }
}

// Vertex inputs and pixel shader outputs, from the container (the same data
// XenosRecomp declares the entry point from). Vertex inputs XenosRecomp's
// location table does not cover get free locations from 16 up.
bool CollectStageInterface(const uint8_t* container, TranslateResult& r) {
  const auto* shader_container = reinterpret_cast<const ShaderContainer*>(container);
  const auto* shader = reinterpret_cast<const Shader*>(container + shader_container->shaderOffset);
  if (r.is_pixel) {
    r.ps_outputs = uint32_t(reinterpret_cast<const PixelShader*>(shader)->outputs.get()) & 0x1F;
    return true;
  }
  const auto* vertex_shader = reinterpret_cast<const VertexShader*>(shader);
  // The entry point's input declarations, in vertex element order.
  static const std::regex kInput(
      R"(\t(\[\[vk::location\((\d+)\)\]\] )?in (float4|uint4) i\w+ : \w+,\n)");
  std::string& hlsl = r.hlsl;
  const size_t main_at = hlsl.find("void main(");
  if (main_at == std::string::npos) {
    return false;
  }
  std::vector<std::smatch> inputs;
  for (std::sregex_iterator it(hlsl.cbegin() + main_at, hlsl.cend(), kInput), end; it != end;
       ++it) {
    inputs.push_back(*it);
  }
  const uint32_t count = vertex_shader->vertexElementCount;
  if (inputs.size() != count) {
    return false;
  }
  uint32_t used = 0;
  for (const auto& m : inputs) {
    if (m[2].matched) used |= 1u << std::stoul(m[2]);
  }
  std::string out(hlsl.cbegin(), hlsl.cbegin() + main_at);
  auto copied = hlsl.cbegin() + main_at;
  for (uint32_t i = 0; i < count; ++i) {
    union {
      VertexElement element;
      uint32_t value;
    };
    value = vertex_shader->vertexElementsAndInterpolators[vertex_shader->field18 + i];
    const std::smatch& m = inputs[i];
    uint32_t location;
    if (m[2].matched) {
      location = uint32_t(std::stoul(m[2]));
    } else {
      location = 16;
      while (location < 32 && (used & (1u << location))) ++location;
      if (location >= 32) {
        return false;
      }
      used |= 1u << location;
    }
    out.append(copied, m[0].first);
    out += m[2].matched ? std::string(m[0]) : "\t[[vk::location(" + std::to_string(location) +
                                                 ")]] " + std::string(m[0]).substr(1);
    copied = m[0].second;
    mantra::shader_cache::VertexElement packed;
    packed.address = uint32_t(element.address);
    packed.location = location;
    packed.usage = mantra::shader_cache::DeclUsage(uint32_t(element.usage));
    packed.usage_index = uint32_t(element.usageIndex);
    packed.is_uint = m[3] == "uint4";
    r.vertex_elements.push_back(packed.Pack());
  }
  out.append(copied, hlsl.cend());
  hlsl = std::move(out);
  return true;
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
  if (!CollectStageInterface(container, result)) {
    return result;
  }

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
