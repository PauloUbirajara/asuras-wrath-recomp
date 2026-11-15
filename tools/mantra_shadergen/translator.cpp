#include "translator.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

  // XenosRecomp lowers LOG/RCP/RSQ (and their _CLAMP/_FF forms) to
  // clamp(op(x), FLT_MIN, FLT_MAX). FLT_MIN is the smallest positive float,
  // so negative results were lost: log2 of anything below 1 became ~0 (every
  // exp2(log2(x) * n) power term evaluated to 1, e.g. specular/fresnel
  // reflections at full strength) and rcp of a negative value turned
  // positive. Xenos clamps infinities to +-FLT_MAX instead.
  // LOG and RSQ also take the absolute value of their operand (as D3D9's
  // log/rsq do); HLSL's log2/rsqrt return NaN for negative inputs, which the
  // FLT_MIN clamp used to hide.
  for (const char* fn : {"clamp(log2(", "clamp(rsqrt("}) {
    const size_t fn_len = std::strlen(fn);
    for (size_t at = 0; (at = hlsl.find(fn, at)) != std::string::npos;) {
      const size_t end = hlsl.find("), FLT_MIN, FLT_MAX)", at);
      if (end == std::string::npos) break;
      hlsl.insert(end, ")");
      hlsl.insert(at + fn_len, "abs(");
      at = end + 5;
    }
  }
  for (size_t at = 0; (at = hlsl.find(", FLT_MIN, FLT_MAX)", at)) != std::string::npos;) {
    hlsl.replace(at, 19, ", -FLT_MAX, FLT_MAX)");
    at += 20;
  }

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
    // Invariant position: the depth prepass, base pass and additive light
    // passes are separate vertex shaders running the same position math, and
    // depth-test each other for equality (GEQUAL, no depth write). Without
    // `precise` (SPIR-V NoContraction) the driver fuses mul+add into FMAs
    // differently per shader, the depths differ in the last bits and the
    // light passes fail on most pixels (unlit, ambient-only characters, and
    // z-fighting speckles).
    const std::string kPos = "\tout float4 oPos : SV_Position";
    if (const size_t at = hlsl.find(kPos); at != std::string::npos) {
      hlsl.replace(at, kPos.size(), "\tout precise float4 oPos : SV_Position");
    }
  }
}

// The vertex fetches' index register is normally r0.x, the vertex index
// XenosRecomp's inputs stand for. Particle shaders instead fetch at r0.y =
// vertex index * (1 / K) [+ b], b < 1 / K: one element per K vertices (4
// quad corners). Returns K (0 when the fetches use the vertex index or the
// computation isn't recognized).
uint32_t VertexIndexDivisor(const std::string& hlsl, const VertexShader* vertex_shader,
                            const uint8_t* microcode, size_t microcode_dwords) {
  if (!vertex_shader->vertexElementCount) return 0;
  union {
    VertexElement element;
    uint32_t value;
  };
  value = vertex_shader->vertexElementsAndInterpolators[vertex_shader->field18];
  const size_t fetch_dword = size_t(element.address) * 3;
  if (fetch_dword >= microcode_dwords) return 0;
  const uint8_t* w = microcode + fetch_dword * 4;
  const uint32_t word0 = uint32_t(w[0]) << 24 | uint32_t(w[1]) << 16 | uint32_t(w[2]) << 8 | w[3];
  const uint32_t src = (word0 >> 5) & 0x3F, src_component = (word0 >> 30) & 3;
  if (src == 0 && src_component == 0) return 0;
  const char component = "xyzw"[src_component];
  const size_t main_at = hlsl.find("void main(");
  if (src != 0 || main_at == std::string::npos) {
    std::fprintf(stderr, "vertex fetch index r%u.%c: not handled\n", src, component);
    return 0;
  }
  std::unordered_map<uint32_t, std::array<float, 4>> literals;
  static const std::regex kLiteral(
      R"(\tfloat4 c(\d+) = asfloat\(uint4\((0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+)\)\);)");
  for (std::sregex_iterator it(hlsl.cbegin() + main_at, hlsl.cend(), kLiteral), end; it != end;
       ++it) {
    std::array<float, 4> v;
    for (int i = 0; i < 4; ++i) {
      const uint32_t bits = uint32_t(std::stoul((*it)[2 + i], nullptr, 16));
      std::memcpy(&v[i], &bits, 4);
    }
    literals[uint32_t(std::stoul((*it)[1]))] = v;
  }
  auto literal = [&](const std::string& reg, char c, float& out) {
    const auto found = literals.find(uint32_t(std::stoul(reg)));
    if (found == literals.end()) return false;
    out = found->second[std::string("xyzw").find(c)];
    return true;
  };
  // The first write of r0.<component>: "r0.<mask> = r0.<x..> * cA.<s> [+ cB.<s>];"
  // or "ps = cA.<c> * r0.x;\n\t\t\tr0.<component> = ps;".
  float scale = 0.0f, bias = 0.0f;
  bool found = false;
  static const std::regex kMad(
      R"(\br0\.([xyzw]+) = r0\.([x]+) \* c(\d+)\.([xyzw]+)(?: \+ c(\d+)\.([xyzw]+))?;)");
  static const std::regex kPs(R"(\bps = c(\d+)\.([xyzw]) \* r0\.x;\s*r0\.([xyzw]) = ps;)");
  static const std::regex kWrite(R"(\br0\.([xyzw]+) =)");
  for (std::sregex_iterator it(hlsl.cbegin() + main_at, hlsl.cend(), kWrite), end; it != end;
       ++it) {
    const std::string mask = (*it)[1];
    const size_t lane = mask.find(component);
    if (lane == std::string::npos) continue;
    std::smatch m;
    const auto from = hlsl.cbegin() + main_at + it->position();
    if (std::regex_search(from, hlsl.cend(), m, kMad, std::regex_constants::match_continuous) &&
        literal(m[3], std::string(m[4])[lane], scale) &&
        (!m[5].matched || literal(m[5], std::string(m[6])[lane], bias))) {
      found = true;
    }
    break;
  }
  if (!found) {
    std::smatch m;
    if (std::regex_search(hlsl.cbegin() + main_at, hlsl.cend(), m, kPs) &&
        std::string(m[3])[0] == component && literal(m[1], std::string(m[2])[0], scale)) {
      found = true;
    }
  }
  const float divisor = scale > 0.0f ? 1.0f / scale : 0.0f;
  const uint32_t k = uint32_t(divisor + 0.5f);
  if (!found || k < 2 || std::abs(divisor - float(k)) > 1e-3f * k || bias < 0.0f ||
      bias * k >= 1.0f) {
    std::fprintf(stderr, "vertex fetch index r0.%c: unrecognized (scale %g bias %g)\n", component,
                 scale, bias);
    return 0;
  }
  return k;
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
    std::string declaration = m[2].matched ? std::string(m[0])
                                           : "\t[[vk::location(" + std::to_string(location) +
                                                 ")]] " + std::string(m[0]).substr(1);
    // XenosRecomp declares BLENDINDICES uint4 and only converts it to float,
    // but games put other data there (Asura's Wrath: normalized 8:8:8:8 bone
    // weights); float4 lets the host vertex format convert it as fetched.
    bool is_uint = m[3] == "uint4";
    if (is_uint && declaration.find(": BLENDINDICES") != std::string::npos) {
      declaration.replace(declaration.find("uint4"), 5, "float4");
      is_uint = false;
    }
    out += declaration;
    copied = m[0].second;
    mantra::shader_cache::VertexElement packed{};
    packed.address = uint32_t(element.address);
    packed.location = location;
    packed.usage = mantra::shader_cache::DeclUsage(uint32_t(element.usage));
    packed.usage_index = uint32_t(element.usageIndex);
    packed.is_uint = is_uint;
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

uint32_t ContainerInterpolatorCount(const std::vector<uint8_t>& container) {
  const auto* shader_container = reinterpret_cast<const ShaderContainer*>(container.data());
  const auto* shader =
      reinterpret_cast<const Shader*>(container.data() + shader_container->shaderOffset);
  return (shader->interpolatorInfo >> 5) & 0x1F;
}

int MaxInterpolatorExport(const std::vector<uint8_t>& microcode) {
  const size_t dwords = microcode.size() / 4;
  auto word = [&](size_t i) {
    const uint8_t* w = microcode.data() + i * 4;
    return uint32_t(w[0]) << 24 | uint32_t(w[1]) << 16 | uint32_t(w[2]) << 8 | w[3];
  };
  const size_t instructions = dwords / 3;
  std::vector<int8_t> is_alu(instructions, 0);
  size_t first = instructions;
  for (size_t cf = 0; cf < first && cf * 3 + 2 < dwords; ++cf) {
    const uint64_t a = word(cf * 3), b = word(cf * 3 + 1), c = word(cf * 3 + 2);
    const uint64_t pair[2] = {a | (b & 0xFFFF) << 32, (b >> 16) | c << 16};
    for (const uint64_t instr : pair) {
      const uint32_t opcode = uint32_t(instr >> 44) & 0xF;
      if (opcode < 1 || opcode > 8 || opcode == 5 || opcode == 6) continue;  // exec variants
      const uint32_t address = uint32_t(instr) & 0xFFF, count = uint32_t(instr >> 12) & 7,
                     sequence = uint32_t(instr >> 16) & 0xFFF;
      if (!count) continue;
      first = std::min<size_t>(first, address);
      for (uint32_t i = 0; i < count && address + i < instructions; ++i) {
        is_alu[address + i] = ((sequence >> (i * 2)) & 1) == 0;
      }
    }
  }
  int highest = -1;
  for (size_t i = 0; i < instructions; ++i) {
    if (!is_alu[i]) continue;
    const uint32_t w0 = word(i * 3);
    if (!((w0 >> 15) & 1)) continue;
    const uint32_t vector_dest = w0 & 0x3F, scalar_dest = (w0 >> 8) & 0x3F;
    if (((w0 >> 16) & 0xF) && vector_dest < 16) highest = std::max(highest, int(vector_dest));
    if (((w0 >> 20) & 0xF) && scalar_dest < 16) highest = std::max(highest, int(scalar_dest));
  }
  return highest;
}

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
  if (!result.is_pixel) {
    // Xenos vertex shaders start with the vertex index in r0.x (billboards and
    // trails expand quads from it); XenosRecomp only provides it in its
    // Unleashed build.
    const size_t main_at = result.hlsl.find("void main(\n");
    const size_t r0_at = result.hlsl.find("\tfloat4 r0 = 0.0;\n", main_at);
    if (main_at != std::string::npos && r0_at != std::string::npos) {
      result.hlsl.replace(r0_at, 18,
                          "\tfloat4 r0 = float4(float(iMantraVertexId + g_MantraVertexIndexBase), "
                          "0.0, 0.0, 0.0);\n");
      result.hlsl.insert(main_at + 11, "\tin uint iMantraVertexId : SV_VertexID,\n");
    }
  }

  {
    // A shader's literal constants (def c250..c255 and the like) are not in
    // the guest constant registers. XenosRecomp uses them as local cN, except
    // where the constant table declares an array over those registers (shaders
    // with relative addressing): there it reads ARRAY(i) from the registers.
    // Point constant-index reads of literal registers back at the literal.
    const size_t main_at = result.hlsl.find("void main(");
    if (main_at != std::string::npos) {
      std::unordered_map<uint32_t, bool> literals;
      static const std::regex literal_re("\\tfloat4 c(\\d+) = asfloat\\(");
      for (auto it = std::sregex_iterator(result.hlsl.begin() + main_at, result.hlsl.end(),
                                          literal_re);
           it != std::sregex_iterator(); ++it) {
        literals[uint32_t(std::stoul((*it)[1].str()))] = true;
      }
      static const std::regex array_re(
          "#define (\\w+)\\(INDEX\\) select\\(\\(INDEX\\) < \\d+, MantraLoad_float4\\(g_PushConstants"
          "\\.\\w+ \\+ \\((\\d+) \\+ min");
      std::unordered_map<std::string, uint32_t> arrays;
      for (auto it = std::sregex_iterator(result.hlsl.begin(), result.hlsl.begin() + main_at,
                                          array_re);
           it != std::sregex_iterator(); ++it) {
        arrays[(*it)[1].str()] = uint32_t(std::stoul((*it)[2].str()));
      }
      if (!literals.empty() && !arrays.empty()) {
        static const std::regex use_re("\\b(\\w+)\\((\\d+)\\)");
        std::string body = result.hlsl.substr(main_at), out;
        auto last = body.cbegin();
        for (auto it = std::sregex_iterator(body.begin(), body.end(), use_re);
             it != std::sregex_iterator(); ++it) {
          const auto array = arrays.find((*it)[1].str());
          if (array == arrays.end()) continue;
          const uint32_t reg = array->second + uint32_t(std::stoul((*it)[2].str()));
          if (!literals.count(reg)) continue;
          out.append(last, body.cbegin() + it->position());
          out += "c" + std::to_string(reg);
          last = body.cbegin() + it->position() + it->length();
        }
        out.append(last, body.cend());
        result.hlsl.replace(main_at, std::string::npos, out);
      }
    }
  }

  if (!result.is_pixel) {
    // (After the literal remap: the index computation reads literals.)
    const auto* shader_container = reinterpret_cast<const ShaderContainer*>(container);
    const auto* shader =
        reinterpret_cast<const Shader*>(container + shader_container->shaderOffset);
    result.vertex_index_divisor = VertexIndexDivisor(
        result.hlsl, reinterpret_cast<const VertexShader*>(shader),
        container + shader_container->virtualSize + shader->physicalOffset, shader->size / 4);
  }

  if (result.is_pixel) {
    // Fixed-point 16-bit render targets are hosted as SNORM value / 32;
    // Vulkan clamps outputs to -1...1 before blending, so scale in the shader.
    // 7e3 float targets are hosted as RGBA16F: clamp to the 7e3 range
    // (0...31.875, alpha 0...1, NaN -> 0) like the guest conversion does.
    const size_t main_at = result.hlsl.find("void main(");
    if (main_at != std::string::npos) {
      std::string scale;
      for (int i = 0; i < 4; ++i) {
        const std::string n = std::to_string(i);
        if (result.hlsl.find("oC" + n + " : SV_Target" + n, main_at) != std::string::npos)
          scale += "if (g_MantraFixed16OutputMask & " + std::to_string(1 << i) + "u) oC" + n +
                   " *= 0.03125; " + "if (g_MantraFloat7e3OutputMask & " +
                   std::to_string(1 << i) + "u) oC" + n + " = select(oC" + n +
                   " > 0.0, min(oC" + n + ", float4(31.875, 31.875, 31.875, 1.0)), 0.0); ";
      }
      if (!scale.empty()) {
        const std::string replacement = "{ " + scale + "return; }";
        for (size_t at = result.hlsl.find("return;", main_at); at != std::string::npos;
             at = result.hlsl.find("return;", at + replacement.size())) {
          result.hlsl.replace(at, 7, replacement);
        }
      }
    }
  }

  // Debugging (with --only <hash>): compile this file instead of the generated
  // HLSL, e.g. an edited copy of the --hlsl output.
  if (const char* patch = std::getenv("MANTRA_HLSL_PATCH_FILE"); patch && *patch) {
    if (std::FILE* file = std::fopen(patch, "rb")) {
      std::string text;
      char buffer[65536];
      for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) text.append(buffer, n);
      std::fclose(file);
      result.hlsl = std::move(text);
    }
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
