#include "translator.h"

#include <cstring>
#include <iterator>
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

  // XenosRecomp has no Texture1D descriptor indices: a 1D fetch uses the same
  // slot as its sampler (tfetch1D reads the slot's 2D view).
  static const std::regex kTexture1DIndex(R"((\w+)_Texture1DDescriptorIndex\b)");
  hlsl = std::regex_replace(hlsl, kTexture1DIndex, "$1_SamplerDescriptorIndex");

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
  static const std::regex kUndeclaredBool(R"(if \(b(\d+) ([!=])= 0\))");
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
  // Bools the constant table does not declare are tested by microcode
  // register: VS b0..b127, PS b128..b255. g_Booleans holds VS b0..15 in bits
  // 0..15 and PS b128..143 in bits 16..31; others can't be expressed.
  {
    std::string rewritten;
    auto from = hlsl.cbegin();
    for (std::sregex_iterator it(hlsl.begin(), hlsl.end(), kUndeclaredBool), end; it != end;
         ++it) {
      const std::smatch& m = *it;
      const uint32_t reg = uint32_t(std::stoul(m[1]));
      const uint32_t base = r.is_pixel ? 128 : 0;
      rewritten.append(from, m[0].first);
      if (reg >= base && reg < base + 16) {
        const uint32_t bit = reg - base + (r.is_pixel ? 16 : 0);
        rewritten += "if ((g_Booleans & (1u << " + std::to_string(bit) + ")) " +
                     std::string(m[2]) + "= 0)";
        r.bool_mask |= 1u << (reg - base);
      } else {
        rewritten += m[0];
        r.bool_overflow = true;
      }
      from = m[0].second;
    }
    rewritten.append(from, hlsl.cend());
    hlsl = std::move(rewritten);
  }

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
  const uint8_t* microcode = container + shader_container->virtualSize + shader->physicalOffset;
  const size_t microcode_dwords = shader->size / 4;
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
    mantra::shader_cache::VertexElement packed{};
    packed.address = uint32_t(element.address);
    packed.location = location;
    packed.usage = mantra::shader_cache::DeclUsage(uint32_t(element.usage));
    packed.usage_index = uint32_t(element.usageIndex);
    packed.is_uint = m[3] == "uint4";
    // The fetch's destination register (vfetch word 0 bits 12-17).
    const size_t fetch_dword = size_t(element.address) * 3;
    if (fetch_dword < microcode_dwords) {
      const uint8_t* w = microcode + fetch_dword * 4;
      packed.dest = ((uint32_t(w[0]) << 24 | uint32_t(w[1]) << 16 | uint32_t(w[2]) << 8 | w[3]) >> 12) & 0x3F;
    }
    r.vertex_elements.push_back(packed.Pack());
  }
  out.append(copied, hlsl.cend());
  hlsl = std::move(out);
  return true;
}

// The hardware links VS exports to PS inputs by interpolator register (the
// D3D runtime patches the VS so its exports land where the PS reads them),
// but XenosRecomp names both sides by semantic, so DXC would link them by
// declaration order of the semantics. Rename both sides to TEXCOORD<register>:
// a VS export to interpolator i (XenosRecomp names it after interpolator
// table entry i) and a PS register rN loaded from an interpolator.
void LinkInterpolatorsByRegister(const uint8_t* container, TranslateResult& r) {
  static constexpr const char* kUsageNames[] = {
      "Position", "BlendWeight", "BlendIndices", "Normal", "PointSize", "TexCoord", "Tangent",
      "Binormal", "TessFactor",  "PositionT",    "Color",  "Fog",       "Depth",    "Sample"};
  std::string& hlsl = r.hlsl;
  const size_t main_at = hlsl.find("void main(");
  if (main_at == std::string::npos) {
    return;
  }
  const size_t body_at = hlsl.find('{', main_at);
  if (body_at == std::string::npos) {
    return;
  }
  if (r.is_pixel) {
    static const std::regex kLoad(R"(float4 r(\d+) = i[A-Za-z]+\d+;)");
    std::string body = hlsl.substr(body_at);
    std::string out;
    auto from = body.cbegin();
    for (std::sregex_iterator it(body.cbegin(), body.cend(), kLoad), end; it != end; ++it) {
      const std::smatch& m = *it;
      out.append(from, m[0].first);
      const uint32_t reg = uint32_t(std::stoul(m[1]));
      out += reg < 16 ? "float4 r" + std::string(m[1]) + " = iTexCoord" + std::string(m[1]) + ";"
                      : std::string(m[0]);
      from = m[0].second;
    }
    out.append(from, body.cend());
    hlsl = hlsl.substr(0, body_at) + out;
    return;
  }
  const auto* shader_container = reinterpret_cast<const ShaderContainer*>(container);
  const auto* vertex_shader =
      reinterpret_cast<const VertexShader*>(container + shader_container->shaderOffset);
  const uint32_t count = (uint32_t(vertex_shader->interpolatorInfo) >> 5) & 0x1F;
  std::unordered_map<std::string, uint32_t> names;
  for (uint32_t i = 0; i < count && i < 16; ++i) {
    union {
      Interpolator interpolator;
      uint32_t value;
    };
    value = vertex_shader->vertexElementsAndInterpolators[vertex_shader->field18 +
                                                         vertex_shader->vertexElementCount + i];
    const uint32_t usage = uint32_t(interpolator.usage);
    if (usage < std::size(kUsageNames)) {
      names.emplace(std::string("o") + kUsageNames[usage] +
                        std::to_string(uint32_t(interpolator.usageIndex)),
                    i);
    }
  }
  static const std::regex kOutput(R"(\bo[A-Z][A-Za-z]*\d+\b)");
  std::string body = hlsl.substr(body_at);
  std::string out;
  auto from = body.cbegin();
  for (std::sregex_iterator it(body.cbegin(), body.cend(), kOutput), end; it != end; ++it) {
    const std::smatch& m = *it;
    out.append(from, m[0].first);
    auto found = names.find(m[0]);
    out += found != names.end() ? "oTexCoord" + std::to_string(found->second) : std::string(m[0]);
    from = m[0].second;
  }
  out.append(from, body.cend());
  hlsl = hlsl.substr(0, body_at) + out;
}

}  // namespace

bool ReplaceMicrocode(std::vector<uint8_t>& container, const std::vector<uint8_t>& microcode) {
  if (container.size() < sizeof(ShaderContainer)) {
    return false;
  }
  const auto* shader_container = reinterpret_cast<const ShaderContainer*>(container.data());
  const size_t shader_at = shader_container->shaderOffset;
  if (shader_at + sizeof(Shader) > container.size()) {
    return false;
  }
  const auto* shader = reinterpret_cast<const Shader*>(container.data() + shader_at);
  const size_t at = size_t(shader_container->virtualSize) + shader->physicalOffset;
  if (uint32_t(shader->size) != microcode.size() || at + microcode.size() > container.size()) {
    return false;
  }
  std::memcpy(container.data() + at, microcode.data(), microcode.size());
  return true;
}

TranslateResult TranslateContainer(const uint8_t* container) {
  TranslateResult result;
  ShaderRecompiler recompiler;
  recompiler.recompile(container, kShaderCommonHlsl);
  result.is_pixel = recompiler.isPixelShader;
  result.spec_constants_mask = recompiler.specConstantsMask;
  result.hlsl = std::move(recompiler.out);
  if (!result.is_pixel) {
    result.hlsl.insert(0, "#define MANTRA_VERTEX_SHADER 1\n");
  }
  AdaptInterface(result);
  LinkInterpolatorsByRegister(container, result);
  if (!CollectStageInterface(container, result)) {
    result.failure = TranslateResult::Failure::kStageInterface;
    return result;
  }

  static DxcCompiler dxc;
  IDxcBlob* spirv = dxc.compile(result.hlsl, result.is_pixel, false, true);
  if (!spirv) {
    result.failure = TranslateResult::Failure::kDxc;
    return result;
  }
  const auto* bytes = static_cast<const uint8_t*>(spirv->GetBufferPointer());
  result.spirv.assign(bytes, bytes + spirv->GetBufferSize());
  spirv->Release();
  result.ok = true;
  return result;
}

}  // namespace mantra_shadergen
