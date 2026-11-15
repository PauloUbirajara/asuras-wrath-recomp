// Guest draws: the translated shaders from the shader cache, a pipeline built
// from the guest registers (draw_pipelines.cpp), dynamic rendering into the
// host render targets, and vertex/index data from the buffer cache
// (buffer_cache.cpp), or copied into the upload ring when it can't be cached.
//
// Interim simplifications (later Phase 1 steps):
//   - textures are the white placeholders;
//   - EDRAM aliasing between render targets is not modeled;
//   - gamma render targets store the shader output unconverted.
#include "mantra_log.h"
#include "native_command_processor.h"

#include <algorithm>
#include <bit>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <rex/graphics/format/ucode.h>
#include <rex/math.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/system/xmemory.h>
#include <rex/ui/vulkan/device.h>

namespace rex::graphics::native {

namespace {

using mantra::shader_cache::DeclUsage;

constexpr uint32_t kGuestPhysicalMemoryMask = 0x1FFFFFFF;
// Largest per-draw vertex copy into the ring, for data the buffer cache can't
// hold; bigger draws are skipped.
constexpr uint32_t kMaxVertexUpload = 16u << 20;

VkFormat VertexAttributeFormat(const MantraShader::VertexInput& in, bool& packed_normal) {
  using F = xenos::VertexFormat;
  const bool s = in.is_signed;
  if (in.is_uint) {
    // XenosRecomp declares NORMAL/TANGENT/BINORMAL/BLENDINDICES as uint4 and
    // reinterprets them in the shader (asfloat, or the packed 11:11:10 decode).
    switch (in.format) {
      case F::k_8_8_8_8: return s ? VK_FORMAT_R8G8B8A8_SINT : VK_FORMAT_R8G8B8A8_UINT;
      case F::k_2_10_10_10:
        return s ? VK_FORMAT_A2B10G10R10_SINT_PACK32 : VK_FORMAT_A2B10G10R10_UINT_PACK32;
      case F::k_10_11_11:
      case F::k_11_11_10: packed_normal = true; return VK_FORMAT_R32_UINT;
      case F::k_16_16:
      case F::k_16_16_FLOAT: return s ? VK_FORMAT_R16G16_SINT : VK_FORMAT_R16G16_UINT;
      case F::k_16_16_16_16:
      case F::k_16_16_16_16_FLOAT:
        return s ? VK_FORMAT_R16G16B16A16_SINT : VK_FORMAT_R16G16B16A16_UINT;
      case F::k_32:
      case F::k_32_FLOAT: return VK_FORMAT_R32_UINT;
      case F::k_32_32:
      case F::k_32_32_FLOAT: return VK_FORMAT_R32G32_UINT;
      case F::k_32_32_32_FLOAT: return VK_FORMAT_R32G32B32_UINT;
      case F::k_32_32_32_32:
      case F::k_32_32_32_32_FLOAT: return VK_FORMAT_R32G32B32A32_UINT;
      default: return VK_FORMAT_UNDEFINED;
    }
  }
  const bool n = in.normalized;
  switch (in.format) {
    case F::k_8_8_8_8:
      return n ? (s ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM)
               : (s ? VK_FORMAT_R8G8B8A8_SSCALED : VK_FORMAT_R8G8B8A8_USCALED);
    case F::k_2_10_10_10:
      return n ? (s ? VK_FORMAT_A2B10G10R10_SNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32)
               : (s ? VK_FORMAT_A2B10G10R10_SSCALED_PACK32
                    : VK_FORMAT_A2B10G10R10_USCALED_PACK32);
    case F::k_16_16:
      return n ? (s ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM)
               : (s ? VK_FORMAT_R16G16_SSCALED : VK_FORMAT_R16G16_USCALED);
    case F::k_16_16_16_16:
      return n ? (s ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_UNORM)
               : (s ? VK_FORMAT_R16G16B16A16_SSCALED : VK_FORMAT_R16G16B16A16_USCALED);
    case F::k_16_16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case F::k_16_16_16_16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case F::k_32_32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case F::k_32_32_32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
    case F::k_32_32_32_32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    // 32-bit integers and 10:11:11 into a float input have no fixed-function
    // equivalent.
    default: return VK_FORMAT_UNDEFINED;
  }
}

// Dwords one element of `format` occupies, or 0 if DecodeVertexElement can't
// decode it.
uint32_t VertexElementWords(xenos::VertexFormat format) {
  using F = xenos::VertexFormat;
  switch (format) {
    case F::k_8_8_8_8:
    case F::k_2_10_10_10:
    case F::k_10_11_11:
    case F::k_11_11_10:
    case F::k_16_16:
    case F::k_16_16_FLOAT:
    case F::k_32_FLOAT: return 1;
    case F::k_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:
    case F::k_32_32_FLOAT: return 2;
    case F::k_32_32_32_FLOAT: return 3;
    case F::k_32_32_32_32_FLOAT: return 4;
    default: return 0;
  }
}

// One vertex element (endian-swapped dwords) to float4, as the vfetch would
// return it: signedness, normalization and exp_adjust applied; components
// the format lacks are 0 (Y, Z) and 1 (W).
void DecodeVertexElement(const MantraShader::VertexInput& in, const uint32_t* w, float* o) {
  using F = xenos::VertexFormat;
  o[0] = o[1] = o[2] = 0.0f;
  o[3] = 1.0f;
  auto fixed = [&](uint32_t bits, uint32_t width) {
    if (in.is_signed) {
      const float v = float(int32_t(bits << (32 - width)) >> (32 - width));
      return in.normalized ? std::max(v / float((1u << (width - 1)) - 1), -1.0f) : v;
    }
    return in.normalized ? float(bits) / float((uint64_t(1) << width) - 1) : float(bits);
  };
  auto packed = [&](const uint32_t* widths, uint32_t count) {
    uint32_t shift = 0;
    for (uint32_t c = 0; c < count; ++c) {
      o[c] = fixed((w[0] >> shift) & uint32_t((uint64_t(1) << widths[c]) - 1), widths[c]);
      shift += widths[c];
    }
  };
  switch (in.format) {
    case F::k_8_8_8_8: {
      static constexpr uint32_t kWidths[] = {8, 8, 8, 8};
      packed(kWidths, 4);
    } break;
    case F::k_2_10_10_10: {
      static constexpr uint32_t kWidths[] = {10, 10, 10, 2};
      packed(kWidths, 4);
    } break;
    case F::k_10_11_11: {
      static constexpr uint32_t kWidths[] = {11, 11, 10};
      packed(kWidths, 3);
    } break;
    case F::k_11_11_10: {
      static constexpr uint32_t kWidths[] = {10, 11, 11};
      packed(kWidths, 3);
    } break;
    case F::k_16_16:
    case F::k_16_16_16_16:
      for (uint32_t c = 0; c < (in.format == F::k_16_16 ? 2u : 4u); ++c) {
        o[c] = fixed((w[c >> 1] >> ((c & 1) * 16)) & 0xFFFF, 16);
      }
      break;
    case F::k_16_16_FLOAT:
    case F::k_16_16_16_16_FLOAT:
      for (uint32_t c = 0; c < (in.format == F::k_16_16_FLOAT ? 2u : 4u); ++c) {
        o[c] = rex::xenos_half_to_float(uint16_t(w[c >> 1] >> ((c & 1) * 16)));
      }
      break;
    case F::k_32_FLOAT:
    case F::k_32_32_FLOAT:
    case F::k_32_32_32_FLOAT:
    case F::k_32_32_32_32_FLOAT:
      std::memcpy(o, w, VertexElementWords(in.format) * 4);
      break;
    default: break;
  }
  if (in.exp_adjust) {
    const float scale = std::ldexp(1.0f, in.exp_adjust);
    for (uint32_t c = 0; c < 4; ++c) o[c] *= scale;
  }
}

VkBlendFactor BlendFactor(xenos::BlendFactor f) {
  using B = xenos::BlendFactor;
  switch (f) {
    case B::kZero: return VK_BLEND_FACTOR_ZERO;
    case B::kOne: return VK_BLEND_FACTOR_ONE;
    case B::kSrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case B::kOneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case B::kSrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case B::kOneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case B::kDstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case B::kOneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case B::kDstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case B::kOneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case B::kConstantColor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case B::kOneMinusConstantColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case B::kConstantAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case B::kOneMinusConstantAlpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case B::kSrcAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
  }
  return VK_BLEND_FACTOR_ONE;
}

VkBlendOp BlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kAdd: return VK_BLEND_OP_ADD;
    case xenos::BlendOp::kSubtract: return VK_BLEND_OP_SUBTRACT;
    case xenos::BlendOp::kMin: return VK_BLEND_OP_MIN;
    case xenos::BlendOp::kMax: return VK_BLEND_OP_MAX;
    case xenos::BlendOp::kRevSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
  }
  return VK_BLEND_OP_ADD;
}

VkSampleCountFlagBits SampleCount(xenos::MsaaSamples msaa) {
  switch (msaa) {
    case xenos::MsaaSamples::k2X: return VK_SAMPLE_COUNT_2_BIT;
    case xenos::MsaaSamples::k4X: return VK_SAMPLE_COUNT_4_BIT;
    default: return VK_SAMPLE_COUNT_1_BIT;
  }
}

bool IsBlendable(VkFormat format) {
  return format != VK_FORMAT_R32_SFLOAT && format != VK_FORMAT_R32G32_SFLOAT;
}

}  // namespace

// --- Vertex inputs ---------------------------------------------------------------

void MantraShader::DecodeVertexInputs(std::span<const uint32_t> packed_elements) {
  vertex_inputs.clear();
  const uint32_t* code = ucode_dwords();
  const size_t dword_count = ucode_dword_count();
  const size_t instruction_count = dword_count / 3;

  // Which instruction slots are fetches, from the exec clauses' sequences.
  std::vector<uint8_t> is_fetch(instruction_count, 0);
  size_t first_instruction = instruction_count;
  for (size_t cf = 0; cf < first_instruction && cf * 3 + 2 < dword_count; ++cf) {
    ucode::ControlFlowInstruction pair[2];
    ucode::UnpackControlFlowInstructions(&code[cf * 3], pair);
    for (const auto& instr : pair) {
      switch (instr.opcode()) {
        case ucode::ControlFlowOpcode::kExec:
        case ucode::ControlFlowOpcode::kExecEnd:
        case ucode::ControlFlowOpcode::kCondExec:
        case ucode::ControlFlowOpcode::kCondExecEnd:
        case ucode::ControlFlowOpcode::kCondExecPred:
        case ucode::ControlFlowOpcode::kCondExecPredEnd:
        case ucode::ControlFlowOpcode::kCondExecPredClean:
        case ucode::ControlFlowOpcode::kCondExecPredCleanEnd: {
          const auto& exec = instr.exec;
          if (!exec.count()) {
            break;
          }
          first_instruction = std::min<size_t>(first_instruction, exec.address());
          for (uint32_t i = 0; i < exec.count(); ++i) {
            const size_t address = size_t(exec.address()) + i;
            if (address < instruction_count) {
              is_fetch[address] = (exec.sequence() >> (i * 2)) & 1;
            }
          }
          break;
        }
        default:
          break;
      }
    }
  }
  auto vertex_fetch_at = [&](size_t address, ucode::VertexFetchInstruction& out) {
    if (address >= instruction_count || !is_fetch[address]) {
      return false;
    }
    std::memcpy(&out, &code[address * 3], sizeof(out));
    return out.opcode() == ucode::FetchOpcode::kVertexFetch;
  };

  // Debug summary of every vertex fetch (logged by the draw trace).
  vfetch_debug.clear();
  for (size_t a = first_instruction; a < instruction_count; ++a) {
    ucode::VertexFetchInstruction f;
    if (vertex_fetch_at(a, f)) {
      vfetch_debug += fmt::format(" a{}:r{}/o{}/f{}/s{}{}", a, f.dest(), f.offset(),
                                  uint32_t(f.data_format()),
                                  f.is_mini_fetch() ? 0u : f.stride(),
                                  f.is_mini_fetch() ? "m" : "");
      if (f.exp_adjust()) vfetch_debug += fmt::format("/x{}", f.exp_adjust());
    }
  }
  for (uint32_t packed : packed_elements) {
    const auto element = mantra::shader_cache::VertexElement::Unpack(packed);
    vfetch_debug += fmt::format(" | loc{}:a{}/r{}", element.location, element.address,
                                element.dest);
    VertexInput in;
    in.location = element.location;
    in.usage = element.usage;
    in.is_uint = element.is_uint;
    // The driver may reorder the fetches: find the element's by the register
    // it writes when the one at its container address writes another.
    size_t address = element.address;
    ucode::VertexFetchInstruction fetch;
    if (vertex_fetch_at(address, fetch) && fetch.dest() != element.dest) {
      for (size_t a = first_instruction; a < instruction_count; ++a) {
        ucode::VertexFetchInstruction candidate;
        if (vertex_fetch_at(a, candidate) && !candidate.is_dest_relative() &&
            candidate.dest() == element.dest) {
          address = a;
          break;
        }
      }
    }
    if (vertex_fetch_at(address, fetch)) {
      // A mini fetch takes the fetch constant and stride of the previous
      // full fetch.
      ucode::VertexFetchInstruction full = fetch;
      bool have_full = !fetch.is_mini_fetch();
      for (size_t a = address; !have_full && a-- > 0;) {
        ucode::VertexFetchInstruction candidate;
        if (vertex_fetch_at(a, candidate) && !candidate.is_mini_fetch()) {
          full = candidate;
          have_full = true;
        }
      }
      if (have_full) {
        in.valid = true;
        in.fetch_constant = full.fetch_constant_index();
        in.stride_words = full.stride();
        in.offset_words = fetch.offset();
        in.format = fetch.data_format();
        in.is_signed = fetch.is_signed();
        in.normalized = fetch.is_normalized();
        in.exp_adjust = fetch.exp_adjust();
      }
    }
    vertex_inputs.push_back(in);
  }
}

// --- Textures ---------------------------------------------------------------------

void NativeCommandProcessor::PrepareTextures(const MantraShader* vs, const MantraShader* ps,
                                             TextureBindings& out) {
  std::memset(&out, 0, sizeof(out));
  uint32_t mask = 0;
  if (vs && vs->cache_entry) mask |= vs->cache_entry->texture_slot_mask;
  if (ps && ps->cache_entry) mask |= ps->cache_entry->texture_slot_mask;
  const RegisterFile& regs = *register_file_;
  while (mask) {
    const uint32_t slot = uint32_t(std::countr_zero(mask));
    mask &= mask - 1;
    const auto fetch = regs.GetTextureFetch(slot);
    if (fetch.type != xenos::FetchConstantType::kTexture) {
      continue;
    }
    const mantra::TextureCache::Binding binding = texture_cache_.Get(fetch, *this);
    out.views[binding.dimension][slot] = binding.view;
    out.samplers[slot] = binding.sampler;
    out.exp_bias[slot] = binding.exp_bias;
  }
}

VkDescriptorSet NativeCommandProcessor::GetTextureSet(const TextureBindings& bindings) {
  if (last_texture_set_ && last_texture_set_submission_ == submission_current_ &&
      bindings == last_texture_bindings_) {
    ++guest_draw_stats_.texture_sets_reused;
    return last_texture_set_;
  }
  VkDescriptorSet set = shader_interface_.AllocateTextureSet(bindings.views, bindings.samplers);
  if (!set) {
    return VK_NULL_HANDLE;
  }
  ++guest_draw_stats_.texture_sets;
  last_texture_bindings_ = bindings;
  last_texture_set_ = set;
  last_texture_set_submission_ = submission_current_;
  return set;
}

uint8_t* NativeCommandProcessor::AllocateStaging(uint32_t size, VkBuffer& buffer,
                                                 VkDeviceSize& offset) {
  if (!BeginCommands()) {
    return nullptr;
  }
  uint32_t ring_offset;
  uint8_t* data = shader_interface_.Allocate(size, ring_offset);
  if (!data) {
    // Ring full: nothing of the current draw is recorded yet, so submit.
    ++constant_stats_.ring_full;
    SubmitCommands();
    if (!BeginCommands()) {
      return nullptr;
    }
    data = shader_interface_.Allocate(size, ring_offset);
    if (!data) {
      MANTRA_WARN_ONCE("texture upload of {} bytes doesn't fit the upload ring", size);
      return nullptr;
    }
  }
  buffer = shader_interface_.ring_buffer();
  offset = ring_offset;
  return data;
}

VkCommandBuffer NativeCommandProcessor::BeginTransferCommands() { return BeginNonDrawCommands(); }

// --- Rendering --------------------------------------------------------------------

VkCommandBuffer NativeCommandProcessor::BeginNonDrawCommands() {
  VkCommandBuffer cmd = BeginCommands();
  EndRendering();
  return cmd;
}

void NativeCommandProcessor::EndRendering() {
  if (!rendering_.active) {
    return;
  }
  vulkan_device_->functions().vkCmdEndRendering(CurrentSlot().command_buffer);
  rendering_.active = false;
}

bool NativeCommandProcessor::BeginDrawRendering(VkCommandBuffer cmd, uint32_t color_mask,
                                                bool use_depth, mantra::PipelineKey& key,
                                                VkRect2D& render_area) {
  const RegisterFile& regs = *register_file_;
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const uint32_t pitch = surface_info.surface_pitch;
  const xenos::MsaaSamples msaa = surface_info.msaa_samples;
  const uint64_t frame = submission_current_;

  // What this draw needs.
  struct ColorTarget {
    uint32_t base = 0;
    xenos::ColorRenderTargetFormat format{};
    VkFormat host_format = VK_FORMAT_UNDEFINED;
  } colors[mantra::kMaxColorAttachments];
  uint32_t color_count = 0;
  for (uint32_t i = 0; i < mantra::kMaxColorAttachments; ++i) {
    if (!((color_mask >> (i * 4)) & 0xF)) {
      continue;
    }
    const auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    colors[i].base = info.color_base;
    colors[i].format = info.color_format;
    colors[i].host_format = mantra::RenderTargetPool::HostColorFormat(info.color_format);
    color_count = i + 1;
  }
  const uint32_t depth_base = regs.Get<reg::RB_DEPTH_INFO>().depth_base;
  if (!color_count && !use_depth) {
    return false;
  }

  // Look the targets up without recording anything (creation clears, which
  // must happen outside rendering).
  mantra::RenderTarget* color_rts[mantra::kMaxColorAttachments] = {};
  mantra::RenderTarget* depth_rt = nullptr;
  bool same = rendering_.active;
  auto lookup = [&](bool create) {
    for (uint32_t i = 0; i < color_count; ++i) {
      if (colors[i].host_format == VK_FORMAT_UNDEFINED) continue;
      color_rts[i] = create ? GetColorTarget(cmd, colors[i].base, pitch, msaa,
                                             colors[i].format, frame)
                            : nullptr;
    }
    if (use_depth && create) {
      depth_rt = render_targets_.GetDepth(cmd, depth_base, pitch, msaa, frame);
    }
  };
  if (same) {
    // Cheap check against the open pass: same images and views.
    const auto key_matches = [&](const mantra::RenderTarget* rt, uint32_t base,
                                 mantra::RtClass cls) {
      return rt && rt->key.base_tiles == (base & (xenos::kEdramTileCount - 1)) &&
             rt->key.pitch_px == pitch && rt->key.msaa == msaa && rt->key.cls == cls;
    };
    for (uint32_t i = 0; i < mantra::kMaxColorAttachments && same; ++i) {
      const bool wanted = i < color_count && colors[i].host_format != VK_FORMAT_UNDEFINED;
      if (!wanted) {
        same = !rendering_.color[i];
      } else {
        same = key_matches(rendering_.color[i], colors[i].base,
                           mantra::RenderTargetPool::HostColorClass(colors[i].format)) &&
               rendering_.color_format[i] == colors[i].host_format;
      }
    }
    same = same && (use_depth ? key_matches(rendering_.depth, depth_base, mantra::RtClass::kDepth)
                              : !rendering_.depth);
  }
  if (same) {
    for (uint32_t i = 0; i < color_count; ++i) {
      color_rts[i] = rendering_.color[i];
    }
    depth_rt = rendering_.depth;
  } else {
    EndRendering();
    lookup(true);
  }

  // Pipeline key: attachment formats and samples.
  key.color_count = uint8_t(color_count);
  for (uint32_t i = 0; i < color_count; ++i) {
    key.color_formats[i] = color_rts[i] ? uint32_t(colors[i].host_format) : 0;
  }
  key.depth_format = depth_rt ? uint32_t(render_targets_.depth_format()) : 0;
  key.sample_count = uint8_t(SampleCount(msaa));
  bool any = depth_rt != nullptr;
  for (uint32_t i = 0; i < color_count; ++i) any |= color_rts[i] != nullptr;
  if (!any) {
    return false;
  }

  if (same) {
    render_area = rendering_.area;
    return true;
  }

  // New pass.
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  VkRenderingAttachmentInfo color_attachments[mantra::kMaxColorAttachments] = {};
  for (uint32_t i = 0; i < color_count; ++i) {
    VkRenderingAttachmentInfo& a = color_attachments[i];
    a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (mantra::RenderTarget* rt = color_rts[i]) {
      mantra::TransitionImage(vulkan_device_, cmd, rt->image,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              mantra::kColorAttachmentStage, mantra::kColorAttachmentAccess);
      a.imageView = render_targets_.GetView(*rt, colors[i].host_format);
      width = std::min(width, rt->image.width);
      height = std::min(height, rt->image.height);
    }
  }
  VkRenderingAttachmentInfo depth_attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  if (depth_rt) {
    mantra::TransitionImage(vulkan_device_, cmd, depth_rt->image,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            mantra::kDepthAttachmentStage, mantra::kDepthAttachmentAccess);
    depth_attachment.imageView = render_targets_.GetView(*depth_rt, render_targets_.depth_format());
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    width = std::min(width, depth_rt->image.width);
    height = std::min(height, depth_rt->image.height);
  }
  VkRenderingInfo info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  info.renderArea = {{0, 0}, {width, height}};
  info.layerCount = 1;
  info.colorAttachmentCount = color_count;
  info.pColorAttachments = color_attachments;
  if (depth_rt) {
    info.pDepthAttachment = &depth_attachment;
    info.pStencilAttachment = &depth_attachment;
  }
  vulkan_device_->functions().vkCmdBeginRendering(cmd, &info);
  ++guest_draw_stats_.render_passes;

  rendering_ = {};
  rendering_.active = true;
  for (uint32_t i = 0; i < color_count; ++i) {
    rendering_.color[i] = color_rts[i];
    rendering_.color_format[i] = color_rts[i] ? colors[i].host_format : VK_FORMAT_UNDEFINED;
  }
  rendering_.depth = depth_rt;
  rendering_.area = info.renderArea;
  render_area = info.renderArea;
  return true;
}

// --- Geometry ---------------------------------------------------------------------

NativeCommandProcessor::DrawResult NativeCommandProcessor::UploadIndices(
    xenos::PrimitiveType prim_type, uint32_t index_count, IndexBufferInfo* index_buffer_info,
    bool restart_allowed, GeometryUpload& upload, IndexRange& range) {
  const RegisterFile& regs = *register_file_;
  const bool quad_list = prim_type == xenos::PrimitiveType::kQuadList;
  const bool line_loop = prim_type == xenos::PrimitiveType::kLineLoop;
  const bool guest_indexed = index_buffer_info && index_buffer_info->guest_base;
  range = {};
  range.max = index_count ? index_count - 1 : 0;
  if (!guest_indexed && !quad_list && !line_loop) {
    upload.indexed = false;
    upload.draw_count = index_count;
    return DrawResult::kDrawn;
  }

  // Guest indices go through the buffer cache, keyed by everything the
  // conversion depends on.
  mantra::BufferCache::Key key;
  std::memset(&key, 0, sizeof(key));
  const uint8_t* src = nullptr;
  bool is_32 = false, use_reset = false;
  uint32_t reset_index = 0;
  if (guest_indexed) {
    is_32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    const uint32_t address = index_buffer_info->guest_base & kGuestPhysicalMemoryMask;
    src = memory_->TranslatePhysical<const uint8_t*>(address);
    if (!src) {
      return DrawResult::kSkipped;
    }
    use_reset = regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena != 0;
    reset_index =
        regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx & (is_32 ? 0xFFFFFF : 0xFFFF);
    range.restart = use_reset && restart_allowed;
    key.start = address;
    key.length = index_count * (is_32 ? 4 : 2);
    key.kind = mantra::BufferCache::Kind::kIndex;
    key.params[0] = index_count;
    key.params[1] = uint32_t(is_32) | (uint32_t(index_buffer_info->endianness) << 1) |
                    (uint32_t(use_reset) << 4) | (uint32_t(range.restart) << 5) |
                    (uint32_t(quad_list) << 6) | (uint32_t(line_loop) << 7);
    key.params[2] = use_reset ? reset_index : 0;
    mantra::BufferCache::Data data;
    if (buffer_cache_.Lookup(key, data)) {
      range.min = data.meta[0];
      range.max = data.meta[1];
      upload.indexed = true;
      upload.index_buffer = data.buffer;
      upload.index_offset = data.offset;
      upload.draw_count = data.meta[2];
      upload.index_type = data.meta[3] ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
      return DrawResult::kDrawn;
    }
  }

  // Indices: guest indices (endian-swapped) or a generated sequence when the
  // primitive type has to be converted.
  std::vector<uint32_t> indices(index_count);
  if (guest_indexed) {
    range.min = UINT32_MAX;
    range.max = 0;
    for (uint32_t i = 0; i < index_count; ++i) {
      uint32_t v;
      if (is_32) {
        uint32_t raw;
        std::memcpy(&raw, src + i * 4, 4);
        v = xenos::GpuSwap(raw, index_buffer_info->endianness) & 0xFFFFFF;
      } else {
        uint16_t raw;
        std::memcpy(&raw, src + i * 2, 2);
        v = xenos::GpuSwap(raw, index_buffer_info->endianness);
      }
      if (use_reset && v == reset_index) {
        indices[i] = UINT32_MAX;
        continue;
      }
      indices[i] = v;
      range.min = std::min(range.min, v);
      range.max = std::max(range.max, v);
    }
    if (range.min > range.max) {
      return DrawResult::kSkipped;  // only reset indices
    }
    if (use_reset && !range.restart) {
      // No host restart for this topology: point reset indices at a valid
      // vertex instead of past the uploaded range.
      for (uint32_t& v : indices) {
        if (v == UINT32_MAX) v = range.min;
      }
    }
  } else {
    for (uint32_t i = 0; i < index_count; ++i) indices[i] = i;
  }
  if (quad_list) {
    std::vector<uint32_t> triangles;
    triangles.reserve(index_count / 4 * 6);
    for (uint32_t q = 0; q + 3 < uint32_t(indices.size()); q += 4) {
      const uint32_t* v = &indices[q];
      triangles.insert(triangles.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
    }
    indices = std::move(triangles);
  } else if (line_loop && !indices.empty()) {
    indices.push_back(indices.front());
  }

  // Host indices are the guest ones; vertex_offset (-min) rebases them onto
  // the vertex bindings, which start at vertex `min`.
  const bool host_32 = range.max > 0xFFFE || range.restart;
  const uint32_t index_size = host_32 ? 4 : 2;
  const uint32_t bytes = std::max<uint32_t>(uint32_t(indices.size()) * index_size, 4);
  mantra::BufferCache::Data data;
  uint8_t* dest = nullptr;
  if (guest_indexed) {
    dest = buffer_cache_.Reserve(key, bytes, submission_current_, data);
  }
  const bool cached = dest != nullptr;
  if (cached) {
    upload.index_buffer = data.buffer;
    upload.index_offset = data.offset;
  } else {
    uint32_t offset;
    dest = shader_interface_.Allocate(bytes, offset);
    if (!dest) {
      return DrawResult::kFailed;  // ring full; the caller submits and retries
    }
    upload.index_buffer = shader_interface_.ring_buffer();
    upload.index_offset = offset;
    guest_draw_stats_.index_bytes += bytes;
  }
  if (host_32) {
    std::memcpy(dest, indices.data(), indices.size() * 4);
  } else {
    auto* out = reinterpret_cast<uint16_t*>(dest);
    for (size_t i = 0; i < indices.size(); ++i) out[i] = uint16_t(indices[i]);
  }
  upload.indexed = true;
  upload.index_type = host_32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
  upload.draw_count = uint32_t(indices.size());
  if (cached) {
    data.meta[0] = range.min;
    data.meta[1] = range.max;
    data.meta[2] = upload.draw_count;
    data.meta[3] = host_32 ? 1 : 0;
    buffer_cache_.Commit(key, data);
  }
  return DrawResult::kDrawn;
}

bool NativeCommandProcessor::CachedVertexData(uint32_t buffer_address, uint32_t buffer_size,
                                              xenos::Endian endian, uint32_t start,
                                              uint32_t size, VkBuffer& buffer,
                                              VkDeviceSize& offset) {
  // Small buffers are cached whole; larger ones (streaming rings, big shared
  // buffers) in 64 KB-aligned spans around what the draw reads.
  constexpr uint32_t kWholeBufferLimit = 1u << 20;
  constexpr uint32_t kSpanAlignment = 64u << 10;
  // Attribute reads may run a little past the last vertex's stride (the
  // vertex extent is offset + 16); that much zero padding follows the data.
  constexpr uint32_t kPadding = 16;
  const uint64_t buffer_end = uint64_t(buffer_address) + buffer_size;
  if (!buffer_size || start < buffer_address || start >= buffer_end) {
    return false;
  }
  uint64_t entry_start = buffer_address, entry_end = buffer_end;
  if (buffer_size > kWholeBufferLimit) {
    entry_start = std::max<uint64_t>(buffer_address, start & ~uint64_t(kSpanAlignment - 1));
    entry_end = std::min<uint64_t>(
        buffer_end, (uint64_t(start) + size + kSpanAlignment - 1) & ~uint64_t(kSpanAlignment - 1));
  }
  if (uint64_t(start) + size > entry_end + kPadding) {
    return false;  // reads past the end of the buffer
  }
  mantra::BufferCache::Key key;
  std::memset(&key, 0, sizeof(key));
  key.start = uint32_t(entry_start);
  key.length = uint32_t(entry_end - entry_start);
  key.kind = mantra::BufferCache::Kind::kVertex;
  key.params[0] = uint32_t(endian);
  mantra::BufferCache::Data data;
  if (!buffer_cache_.Lookup(key, data)) {
    uint8_t* dest = buffer_cache_.Reserve(key, key.length + kPadding, submission_current_, data);
    if (!dest) {
      return false;
    }
    const uint32_t* src = memory_->TranslatePhysical<const uint32_t*>(key.start);
    if (!src) {
      return false;  // the reservation stays invalid
    }
    auto* out = reinterpret_cast<uint32_t*>(dest);
    const uint32_t words = key.length / 4;
    for (uint32_t w = 0; w < words; ++w) {
      out[w] = xenos::GpuSwap(src[w], endian);
    }
    std::memset(dest + words * 4, 0, key.length + kPadding - words * 4);
    buffer_cache_.Commit(key, data);
  }
  buffer = data.buffer;
  offset = data.offset + (start - key.start);
  return true;
}

NativeCommandProcessor::DrawResult NativeCommandProcessor::UploadGeometry(
    const MantraShader& vs, xenos::PrimitiveType prim_type, uint32_t index_count,
    IndexBufferInfo* index_buffer_info, bool restart_allowed, mantra::PipelineKey& key,
    GeometryUpload& upload, uint32_t& spec_constants) {
  const RegisterFile& regs = *register_file_;
  const uint32_t index_base = regs[XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;

  IndexRange range;
  const DrawResult indices =
      UploadIndices(prim_type, index_count, index_buffer_info, restart_allowed, upload, range);
  if (indices != DrawResult::kDrawn) {
    return indices;
  }
  upload.vertex_offset = upload.indexed ? -int32_t(range.min) : 0;
  upload.vertex_index_base = (upload.indexed ? range.min : 0) + index_base;
  key.primitive_restart = range.restart ? 1 : 0;

  // Vertex buffers: one binding per fetch constant, starting at vertex
  // min + VGT_INDX_OFFSET, endian-swapped per dword with the fetch constant's
  // endianness. Served from the buffer cache, or copied into the ring when
  // they can't be cached.
  const uint64_t first_vertex = uint64_t(range.min) + index_base;
  const uint32_t vertex_count = range.max - range.min + 1;
  uint32_t binding_fetch_constants[mantra::kMaxVertexBindings];
  uint32_t binding_count = 0;
  bool packed_normal = false;
  bool need_zero_binding = false;
  uint32_t attribute_count = 0;
  uint32_t zero_binding_attributes[mantra::kMaxVertexAttributes];
  uint32_t zero_attribute_count = 0;
  struct Expanded {
    uint32_t attribute;
    const MantraShader::VertexInput* in;
  };
  Expanded expanded[mantra::kMaxVertexAttributes];
  uint32_t expanded_count = 0;
  for (const MantraShader::VertexInput& in : vs.vertex_inputs) {
    if (attribute_count >= mantra::kMaxVertexAttributes) break;
    auto& attr = key.attributes[attribute_count++];
    attr.location = uint8_t(in.location);
    const VkFormat format = in.valid ? VertexAttributeFormat(in, packed_normal)
                                     : VK_FORMAT_UNDEFINED;
    const auto fetch = in.valid ? regs.GetVertexFetch(in.fetch_constant)
                                : xenos::xe_gpu_vertex_fetch_t{};
    using VF = xenos::VertexFormat;
    const bool packed_format = in.format == VF::k_10_11_11 || in.format == VF::k_11_11_10;
    // XenosRecomp's uint4 inputs (NORMAL, TANGENT, BINORMAL) are asfloat()ed
    // unless they are packed 10:11:11: give them the fetched floats' bits.
    const bool uint_as_float_bits =
        in.is_uint && !packed_format &&
        (in.format == VF::k_8_8_8_8 || in.format == VF::k_2_10_10_10 ||
         in.format == VF::k_16_16 || in.format == VF::k_16_16_16_16 ||
         in.format == VF::k_16_16_FLOAT || in.format == VF::k_16_16_16_16_FLOAT);
    if (in.valid && fetch.type == xenos::FetchConstantType::kVertex && in.offset_words >= 0 &&
        (vs.vertex_index_divisor > 1 ||
         (!in.is_uint && (packed_format || (in.exp_adjust && VertexElementWords(in.format)))) ||
         uint_as_float_bits || (in.is_uint && in.exp_adjust && !packed_format &&
                                VertexElementWords(in.format)))) {
      // No Vulkan vertex format (10:11:11, a scaled fetch, float bits for a
      // uint input), or fetched at vertex index / divisor: decoded on the
      // CPU into a float4 binding.
      if (expanded_count >= mantra::kMaxVertexAttributes) return DrawResult::kSkipped;
      expanded[expanded_count++] = {attribute_count - 1, &in};
      attr.format = uint32_t(in.is_uint ? VK_FORMAT_R32G32B32A32_UINT
                                        : VK_FORMAT_R32G32B32A32_SFLOAT);
      attr.offset = 0;
      continue;
    }
    if (format == VK_FORMAT_UNDEFINED || fetch.type != xenos::FetchConstantType::kVertex ||
        in.offset_words < 0) {
      // Fed zeros from a dedicated binding.
      ++guest_draw_stats_.missing_inputs;
      {
        static std::mutex zero_log_mutex;
        static std::unordered_set<uint64_t> zero_logged;
        std::lock_guard lock(zero_log_mutex);
        if (zero_logged.insert(vs.ucode_data_hash() ^ (uint64_t(in.location) << 58)).second) {
          MANTRA_WARN("zero-fed input: vs {:016X} loc {} usage {} uint {} valid {} fc {} "
                      "fmt {} signed {} norm {} offset {} stride {} fetch type {}",
                      vs.ucode_data_hash(), in.location, uint32_t(in.usage), in.is_uint,
                      in.valid, in.fetch_constant, uint32_t(in.format), in.is_signed,
                      in.normalized, in.offset_words, in.stride_words, uint32_t(fetch.type));
        }
      }
      attr.format = uint32_t(in.is_uint ? VK_FORMAT_R32G32B32A32_UINT
                                        : VK_FORMAT_R32G32B32A32_SFLOAT);
      attr.offset = 0;
      zero_binding_attributes[zero_attribute_count++] = attribute_count - 1;
      need_zero_binding = true;
      continue;
    }
    uint32_t binding = 0;
    while (binding < binding_count && binding_fetch_constants[binding] != in.fetch_constant) {
      ++binding;
    }
    if (binding == binding_count) {
      if (binding_count >= mantra::kMaxVertexBindings - 1) {
        return DrawResult::kSkipped;
      }
      binding_fetch_constants[binding_count] = in.fetch_constant;
      key.binding_strides[binding_count] = in.stride_words * 4;
      ++binding_count;
    }
    attr.binding = uint8_t(binding);
    attr.format = uint32_t(format);
    attr.offset = uint16_t(in.offset_words * 4);
  }
  if (packed_normal) {
    spec_constants |= 1u << 0;  // SPEC_CONSTANT_R11G11B10_NORMAL
  }

  for (uint32_t b = 0; b < binding_count; ++b) {
    const auto fetch = regs.GetVertexFetch(binding_fetch_constants[b]);
    const uint32_t stride = key.binding_strides[b];
    const uint32_t buffer_address = (uint32_t(fetch.address) << 2) & kGuestPhysicalMemoryMask;
    const uint64_t buffer_size = uint64_t(fetch.size) << 2;
    // Bytes the attributes of one vertex reach.
    uint32_t vertex_extent = std::max<uint32_t>(stride, 4);
    for (uint32_t a = 0; a < attribute_count; ++a) {
      if (key.attributes[a].binding == b) {
        vertex_extent = std::max<uint32_t>(vertex_extent, key.attributes[a].offset + 16);
      }
    }
    const uint64_t start = stride ? first_vertex * stride : 0;
    const uint64_t size64 =
        stride ? uint64_t(vertex_count - 1) * stride + vertex_extent : vertex_extent;
    if (start + size64 <= 0xFFFFFFFFu - buffer_address &&
        CachedVertexData(buffer_address, uint32_t(std::min<uint64_t>(buffer_size, 0xFFFFFFFFu)),
                         fetch.endian, uint32_t(buffer_address + start), uint32_t(size64),
                         upload.binding_buffers[b], upload.binding_offsets[b])) {
      continue;
    }
    if (size64 > kMaxVertexUpload) {
      MANTRA_WARN_ONCE("draw with {} bytes of uncacheable vertex data, skipping", size64);
      return DrawResult::kSkipped;
    }
    const uint32_t size = uint32_t((size64 + 3) & ~uint64_t(3));
    uint32_t offset;
    uint8_t* dest = shader_interface_.Allocate(size, offset);
    if (!dest) {
      return DrawResult::kFailed;  // ring full; the caller submits and retries
    }
    const uint32_t* src = memory_->TranslatePhysical<const uint32_t*>(buffer_address);
    const uint64_t available = start < buffer_size ? buffer_size - start : 0;
    const uint32_t copy_words = uint32_t(std::min<uint64_t>(available, size) / 4);
    auto* out = reinterpret_cast<uint32_t*>(dest);
    if (src) {
      src += start / 4;
      for (uint32_t w = 0; w < copy_words; ++w) {
        out[w] = xenos::GpuSwap(src[w], fetch.endian);
      }
    }
    std::memset(out + (src ? copy_words : 0), 0, size - (src ? copy_words : 0) * 4);
    upload.binding_buffers[b] = shader_interface_.ring_buffer();
    upload.binding_offsets[b] = offset;
    guest_draw_stats_.vertex_bytes += size;
  }
  for (uint32_t e = 0; e < expanded_count; ++e) {
    if (binding_count >= mantra::kMaxVertexBindings - 1) {
      return DrawResult::kSkipped;
    }
    const MantraShader::VertexInput& in = *expanded[e].in;
    const auto fetch = regs.GetVertexFetch(in.fetch_constant);
    const uint32_t size = vertex_count * 16;
    if (size > kMaxVertexUpload) {
      MANTRA_WARN_ONCE("draw with {} bytes of expanded vertex data, skipping", size);
      return DrawResult::kSkipped;
    }
    uint32_t offset;
    auto* out = reinterpret_cast<float*>(shader_interface_.Allocate(size, offset));
    if (!out) {
      return DrawResult::kFailed;
    }
    const uint32_t buffer_words = fetch.size;
    const uint32_t buffer_base = fetch.address;  // in dwords
    const uint32_t* membase = memory_->TranslatePhysical<const uint32_t*>(0);
    const uint32_t element_words = VertexElementWords(in.format);
    for (uint32_t v = 0; v < vertex_count; ++v) {
      const uint64_t element = vs.vertex_index_divisor > 1
                                   ? (first_vertex + v) / vs.vertex_index_divisor
                                   : first_vertex + v;
      const uint64_t word = element * in.stride_words + uint64_t(in.offset_words);
      uint32_t words[4] = {};
      for (uint32_t i = 0; i < element_words; ++i) {
        if (word + i < buffer_words) {
          const uint32_t address =
              ((buffer_base + uint32_t(word + i)) << 2) & kGuestPhysicalMemoryMask;
          words[i] = xenos::GpuSwap(membase[address >> 2], fetch.endian);
        }
      }
      if (in.is_uint && (in.format == xenos::VertexFormat::k_10_11_11 ||
                         in.format == xenos::VertexFormat::k_11_11_10)) {
        // Packed normal: the shader unpacks the raw word itself.
        std::memcpy(out + v * 4, words, 4);
      } else {
        DecodeVertexElement(in, words, out + v * 4);
      }
    }
    const uint32_t b = binding_count++;
    key.binding_strides[b] = 16;
    upload.binding_buffers[b] = shader_interface_.ring_buffer();
    upload.binding_offsets[b] = offset;
    key.attributes[expanded[e].attribute].binding = uint8_t(b);
    guest_draw_stats_.vertex_bytes += size;
    ++guest_draw_stats_.expanded_inputs;
  }
  if (need_zero_binding) {
    const uint32_t b = binding_count++;
    uint32_t offset;
    uint8_t* dest = shader_interface_.Allocate(16, offset);
    if (!dest) {
      return DrawResult::kFailed;
    }
    std::memset(dest, 0, 16);
    key.binding_strides[b] = 0;
    upload.binding_buffers[b] = shader_interface_.ring_buffer();
    upload.binding_offsets[b] = offset;
    for (uint32_t i = 0; i < zero_attribute_count; ++i) {
      key.attributes[zero_binding_attributes[i]].binding = uint8_t(b);
    }
  }
  key.binding_count = uint8_t(binding_count);
  key.attribute_count = uint8_t(attribute_count);
  upload.binding_count = binding_count;
  return DrawResult::kDrawn;
}

// --- Draw -------------------------------------------------------------------------

NativeCommandProcessor::DrawResult NativeCommandProcessor::DrawGuest(
    xenos::PrimitiveType prim_type, uint32_t index_count, IndexBufferInfo* index_buffer_info) {
  auto* vs = static_cast<MantraShader*>(active_vertex_shader());
  auto* ps = static_cast<MantraShader*>(active_pixel_shader());
  if (ps && !ps->module &&
      register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kDepthOnly) {
    // Depth-only pass: the pixel shader's color is discarded (the driver
    // binds its untranslated fill shader here), so draw without one.
    ps = nullptr;
  }
  if (!vs || !vs->module || (ps && !ps->module)) {
    ++constant_stats_.no_translation;
    return DrawResult::kSkipped;
  }
  if (!index_count) {
    return DrawResult::kSkipped;
  }
  {
    // Debug: MANTRA_SKIP_SHADERS=<hash>,<hash>... skips draws using any of
    // these vertex or pixel shaders (exact hashes, as traced).
    static const std::unordered_set<uint64_t> skip = [] {
      std::unordered_set<uint64_t> set;
      if (const char* env = std::getenv("MANTRA_SKIP_SHADERS")) {
        for (const char* at = env; *at;) {
          char* end;
          const uint64_t hash = std::strtoull(at, &end, 16);
          if (end == at) break;
          set.insert(hash);
          at = *end ? end + 1 : end;
        }
      }
      return set;
    }();
    if (!skip.empty() &&
        ((vs->cache_entry && skip.count(vs->cache_entry->exact_hash)) ||
         (ps && ps->cache_entry && skip.count(ps->cache_entry->exact_hash)))) {
      return DrawResult::kSkipped;
    }
  }
  const RegisterFile& regs = *register_file_;

  mantra::PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vertex_module = uint64_t(vs->module);
  key.pixel_module = ps ? uint64_t(ps->module) : 0;

  // Primitive type.
  bool restart_allowed = false;
  switch (prim_type) {
    case xenos::PrimitiveType::kPointList: key.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case xenos::PrimitiveType::kLineList: key.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case xenos::PrimitiveType::kLineStrip:
    case xenos::PrimitiveType::kLineLoop:
      key.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      restart_allowed = prim_type == xenos::PrimitiveType::kLineStrip;
      break;
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kQuadList:
      key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
    case xenos::PrimitiveType::kQuadStrip:
      key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      restart_allowed = true;
      break;
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kPolygon:
      key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      restart_allowed = true;
      break;
    case xenos::PrimitiveType::kRectangleList:
      if (!draw_pipelines_.rect_lists_supported()) {
        ++guest_draw_stats_.skipped_primitive;
        return DrawResult::kSkipped;
      }
      key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      key.rect_list = 1;
      break;
    default:
      ++guest_draw_stats_.skipped_primitive;
      MANTRA_WARN_ONCE("unsupported primitive type {}, skipping", uint32_t(prim_type));
      return DrawResult::kSkipped;
  }
  const bool polygonal = key.topology >= VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  // Render targets.
  const auto depth_control = draw_util::GetNormalizedDepthControl(regs);
  const uint32_t color_mask =
      draw_util::GetNormalizedColorMask(regs, ps ? (ps->ps_outputs & 0xF) : 0);
  const bool use_depth = depth_control.z_enable || depth_control.stencil_enable;
  if (!regs.Get<reg::RB_SURFACE_INFO>().surface_pitch) {
    ++guest_draw_stats_.skipped_no_target;
    return DrawResult::kSkipped;
  }

  // Rasterizer.
  const auto mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  key.polygon_mode = VK_POLYGON_MODE_FILL;
  if (polygonal && !key.rect_list) {
    key.cull_mode = (mode.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) |
                    (mode.cull_back ? VK_CULL_MODE_BACK_BIT : 0);
    key.front_face = mode.face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (vulkan_device_->properties().fillModeNonSolid &&
        mode.poly_mode == xenos::PolygonModeEnable::kDualMode) {
      xenos::PolygonType type = xenos::PolygonType::kTriangles;
      if (!mode.cull_front) type = std::min(type, mode.polymode_front_ptype);
      if (!mode.cull_back) type = std::min(type, mode.polymode_back_ptype);
      if (type != xenos::PolygonType::kTriangles) key.polygon_mode = VK_POLYGON_MODE_LINE;
    }
  }
  float bias_slope = 0.0f, bias_constant = 0.0f;
  draw_util::GetPreferredFacePolygonOffset(regs, polygonal, bias_slope, bias_constant);
  key.depth_bias = (bias_slope != 0.0f || bias_constant != 0.0f) && use_depth;

  // Depth / stencil.
  if (use_depth) {
    key.depth_test = depth_control.z_enable;
    key.depth_write = depth_control.z_enable && depth_control.z_write_enable;
    key.depth_compare = uint8_t(depth_control.zfunc);  // same encoding as VkCompareOp
    key.stencil_test = depth_control.stencil_enable;
    if (key.stencil_test) {
      key.stencil_front[0] = uint8_t(depth_control.stencilfail);  // same as VkStencilOp
      key.stencil_front[1] = uint8_t(depth_control.stencilzpass);
      key.stencil_front[2] = uint8_t(depth_control.stencilzfail);
      key.stencil_front[3] = uint8_t(depth_control.stencilfunc);
      if (polygonal && depth_control.backface_enable) {
        key.stencil_back[0] = uint8_t(depth_control.stencilfail_bf);
        key.stencil_back[1] = uint8_t(depth_control.stencilzpass_bf);
        key.stencil_back[2] = uint8_t(depth_control.stencilzfail_bf);
        key.stencil_back[3] = uint8_t(depth_control.stencilfunc_bf);
      } else {
        std::memcpy(key.stencil_back, key.stencil_front, 4);
      }
    }
  }

  // Alpha test (XenosRecomp emulates "alpha >= reference" with clip()).
  const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  uint32_t spec_constants = 0;
  if (color_control.alpha_test_enable &&
      (color_control.alpha_func == xenos::CompareFunction::kGreater ||
       color_control.alpha_func == xenos::CompareFunction::kGreaterEqual)) {
    spec_constants |= 1u << 1;  // SPEC_CONSTANT_ALPHA_TEST
  } else if (color_control.alpha_test_enable &&
             color_control.alpha_func != xenos::CompareFunction::kAlways) {
    MANTRA_WARN_ONCE("alpha test function {} is not supported, ignoring",
                     uint32_t(color_control.alpha_func));
  }
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  key.alpha_to_coverage =
      color_control.alpha_to_mask_enable && surface_info.msaa_samples != xenos::MsaaSamples::k1X;

  // Geometry and constants go into the ring before rendering begins (a full
  // ring submits, which ends rendering). Retried once in a fresh submission.
  GeometryUpload upload;
  mantra::DrawConstantOffsets offsets;
  draw_util::ViewportInfo viewport;
  {
    const auto& props = vulkan_device_->properties();
    // origin_bottom_left negates Y: DXC's -fvk-invert-y negates it again.
    draw_util::GetHostViewportInfo(regs, 1, 1, true, props.maxViewportDimensions[0],
                                   props.maxViewportDimensions[1], true, depth_control, false,
                                   true, ps && (ps->ps_outputs & 0x10), viewport);
  }
  if (!viewport.xy_extent[0] || !viewport.xy_extent[1]) {
    ++guest_draw_stats_.skipped_viewport;
    return DrawResult::kSkipped;
  }
  // Textures first: loading them may record transfers and submit.
  TextureBindings textures;
  PrepareTextures(vs, ps, textures);
  VkDescriptorSet texture_set = VK_NULL_HANDLE;

  mantra::SharedConstants shared;
  std::copy_n(viewport.ndc_scale, 3, shared.ndc_scale);
  std::copy_n(viewport.ndc_offset, 3, shared.ndc_offset);
  {
    // Fetch results are scaled by 2^exp_adjust in the shader (mantraExpAdjust).
    uint32_t mask = 0;
    if (vs->cache_entry) mask |= vs->cache_entry->texture_slot_mask;
    if (ps && ps->cache_entry) mask |= ps->cache_entry->texture_slot_mask;
    while (mask) {
      const uint32_t slot = uint32_t(std::countr_zero(mask));
      mask &= mask - 1;
      shared.descriptor_indices[slot] =
          uint32_t(int32_t(regs.GetTextureFetch(slot).exp_adjust) + textures.exp_bias[slot]);
    }
  }
  // (key.color_count is only filled by BeginDrawRendering, later.)
  for (uint32_t i = 0; i < 4; ++i) {
    const auto rt_format =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]).color_format;
    if (((color_mask >> (i * 4)) & 0xF) && (rt_format == xenos::ColorRenderTargetFormat::k_16_16 ||
                                 rt_format == xenos::ColorRenderTargetFormat::k_16_16_16_16)) {
      shared.fixed16_output_mask |= 1u << i;
    }
    if (((color_mask >> (i * 4)) & 0xF) &&
        (rt_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
         rt_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
      shared.float7e3_output_mask |= 1u << i;
    }
  }
  bool uploaded = false;
  for (int attempt = 0; attempt < 2 && !uploaded; ++attempt) {
    if (!BeginCommands()) {
      return DrawResult::kFailed;
    }
    const uint64_t submission = submission_current_;
    uint32_t attempt_spec = spec_constants;
    const DrawResult geometry = UploadGeometry(*vs, prim_type, index_count, index_buffer_info,
                                               restart_allowed, key, upload, attempt_spec);
    if (geometry == DrawResult::kSkipped) {
      ++guest_draw_stats_.skipped_geometry;
      return geometry;
    }
    if (geometry == DrawResult::kFailed) {
      ++constant_stats_.ring_full;
      SubmitCommands();
      continue;
    }
    shared.vertex_index_base = upload.vertex_index_base;
    if (!PrepareDrawConstants(offsets, shared)) {
      return DrawResult::kFailed;
    }
    if (submission_current_ != submission) {
      continue;
    }
    texture_set = GetTextureSet(textures);
    if (!texture_set) {
      // The slot's descriptor pool is exhausted: start a new submission.
      SubmitCommands();
      continue;
    }
    key.spec_constants = attempt_spec;
    uploaded = true;
  }
  if (!uploaded) {
    return DrawResult::kFailed;
  }

  VkCommandBuffer cmd = BeginCommands();
  VkRect2D render_area;
  if (!BeginDrawRendering(cmd, color_mask, use_depth, key, render_area)) {
    ++guest_draw_stats_.skipped_no_target;
    return DrawResult::kSkipped;
  }

  // Blending (per color attachment).
  for (uint32_t i = 0; i < key.color_count; ++i) {
    auto& b = key.blend[i];
    b.write_mask = key.color_formats[i] ? uint8_t((color_mask >> (i * 4)) & 0xF) : 0;
    const auto blend =
        regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
    b.src_color = uint8_t(BlendFactor(blend.color_srcblend));
    b.dst_color = uint8_t(BlendFactor(blend.color_destblend));
    b.color_op = uint8_t(BlendOp(blend.color_comb_fcn));
    b.src_alpha = uint8_t(BlendFactor(blend.alpha_srcblend));
    b.dst_alpha = uint8_t(BlendFactor(blend.alpha_destblend));
    b.alpha_op = uint8_t(BlendOp(blend.alpha_comb_fcn));
    const bool overwrite = b.src_color == VK_BLEND_FACTOR_ONE &&
                           b.dst_color == VK_BLEND_FACTOR_ZERO && b.color_op == VK_BLEND_OP_ADD &&
                           b.src_alpha == VK_BLEND_FACTOR_ONE &&
                           b.dst_alpha == VK_BLEND_FACTOR_ZERO && b.alpha_op == VK_BLEND_OP_ADD;
    b.enable = b.write_mask && !overwrite && IsBlendable(VkFormat(key.color_formats[i]));
    if (!b.enable) {
      b.src_color = b.dst_color = b.color_op = b.src_alpha = b.dst_alpha = b.alpha_op = 0;
    }
    // Fixed-point 16-bit render targets: the pixel shader scales its output
    // by 1 / 32 (fixed16_output_mask), which source-alpha factors would see.
    if ((shared.fixed16_output_mask >> i) & 1 && b.enable &&
        (b.src_color == VK_BLEND_FACTOR_SRC_ALPHA || b.dst_color == VK_BLEND_FACTOR_SRC_ALPHA ||
         b.src_color == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ||
         b.dst_color == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA)) {
      MANTRA_WARN_ONCE("source-alpha blend on a fixed-point 16-bit target, alpha / 32");
    }
  }

  const VkPipeline pipeline = draw_pipelines_.Get(key);
  if (!pipeline) {
    ++guest_draw_stats_.failed_pipeline;
    return DrawResult::kFailed;
  }

  const auto& dfn = vulkan_device_->functions();
  dfn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  shader_interface_.Bind(cmd, offsets, texture_set);

  // Viewport and scissor (inside the render area).
  VkViewport vk_viewport;
  vk_viewport.x = float(viewport.xy_offset[0]);
  vk_viewport.y = float(viewport.xy_offset[1]);
  vk_viewport.width = float(viewport.xy_extent[0]);
  vk_viewport.height = float(viewport.xy_extent[1]);
  vk_viewport.minDepth = viewport.z_min;
  vk_viewport.maxDepth = viewport.z_max;
  dfn.vkCmdSetViewport(cmd, 0, 1, &vk_viewport);
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  const uint32_t x0 = std::min(scissor.offset[0], render_area.extent.width);
  const uint32_t y0 = std::min(scissor.offset[1], render_area.extent.height);
  const uint32_t x1 = std::min(scissor.offset[0] + scissor.extent[0], render_area.extent.width);
  const uint32_t y1 = std::min(scissor.offset[1] + scissor.extent[1], render_area.extent.height);
  if (x1 <= x0 || y1 <= y0) {
    ++guest_draw_stats_.skipped_viewport;
    return DrawResult::kSkipped;
  }
  const VkRect2D vk_scissor = {{int32_t(x0), int32_t(y0)}, {x1 - x0, y1 - y0}};
  dfn.vkCmdSetScissor(cmd, 0, 1, &vk_scissor);

  // Depth bias, blend constants, stencil.
  const float constant_scale =
      render_targets_.depth_format() == VK_FORMAT_D24_UNORM_S8_UINT
          ? draw_util::kD3D10PolygonOffsetFactorUnorm24
          : draw_util::kD3D10PolygonOffsetFactorFloat24;
  dfn.vkCmdSetDepthBias(cmd, bias_constant * constant_scale, 0.0f,
                        bias_slope * xenos::kPolygonOffsetScaleSubpixelUnit);
  float blend_constants[4] = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  dfn.vkCmdSetBlendConstants(cmd, blend_constants);
  const auto stencil_front = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK);
  const auto stencil_back = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  const bool separate_back = polygonal && depth_control.backface_enable;
  const auto& back = separate_back ? stencil_back : stencil_front;
  dfn.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil_front.stencilmask);
  dfn.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilmask);
  dfn.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil_front.stencilwritemask);
  dfn.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilwritemask);
  dfn.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil_front.stencilref);
  dfn.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilref);

  if (trace_current_draw_) {
    MANTRA_INFO(
        "trace:   state vp {},{} {}x{} z {}..{} ndc ({} {} {}) + ({} {} {}) scissor {},{}-{},{} "
        "area {}x{} cull {} front {} colormask {:X} depth t{} w{} f{} stencil {} ops {}/{}/{}/{} "
        "ref {} mask {} wmask {} a2c {} count {} vofs {} indexed {}",
        viewport.xy_offset[0], viewport.xy_offset[1], viewport.xy_extent[0],
        viewport.xy_extent[1], viewport.z_min, viewport.z_max, viewport.ndc_scale[0],
        viewport.ndc_scale[1], viewport.ndc_scale[2], viewport.ndc_offset[0],
        viewport.ndc_offset[1], viewport.ndc_offset[2], x0, y0, x1, y1,
        render_area.extent.width, render_area.extent.height, uint32_t(key.cull_mode),
        uint32_t(key.front_face), color_mask, uint32_t(key.depth_test),
        uint32_t(key.depth_write), uint32_t(key.depth_compare), uint32_t(key.stencil_test),
        key.stencil_front[0], key.stencil_front[1], key.stencil_front[2], key.stencil_front[3],
        uint32_t(stencil_front.stencilref), uint32_t(stencil_front.stencilmask),
        uint32_t(stencil_front.stencilwritemask), uint32_t(key.alpha_to_coverage),
        upload.draw_count, upload.vertex_offset, uint32_t(upload.indexed));
  }

  // Geometry.
  if (upload.binding_count) {
    dfn.vkCmdBindVertexBuffers(cmd, 0, upload.binding_count, upload.binding_buffers,
                               upload.binding_offsets);
  }
  if (upload.indexed) {
    dfn.vkCmdBindIndexBuffer(cmd, upload.index_buffer, upload.index_offset, upload.index_type);
    dfn.vkCmdDrawIndexed(cmd, upload.draw_count, 1, 0, upload.vertex_offset, 0);
  } else {
    dfn.vkCmdDraw(cmd, upload.draw_count, 1, 0, 0);
  }
  ++guest_draw_stats_.drawn;
  return DrawResult::kDrawn;
}

}  // namespace rex::graphics::native
