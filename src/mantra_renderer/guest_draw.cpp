// Guest draws: the translated shaders from the shader cache, a pipeline built
// from the guest registers (draw_pipelines.cpp), dynamic rendering into the
// host render targets, and vertex/index data copied from guest memory into the
// upload ring for each draw.
//
// Interim simplifications (later Phase 1 steps):
//   - geometry is re-uploaded per draw (no buffer cache yet);
//   - textures are the white placeholders;
//   - EDRAM aliasing between render targets is not modeled;
//   - gamma render targets store the shader output unconverted.
#include "mantra_log.h"
#include "native_command_processor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include <rex/graphics/format/ucode.h>
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
// Largest per-draw vertex upload; bigger draws are skipped (no buffer cache
// yet).
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

  for (uint32_t packed : packed_elements) {
    const auto element = mantra::shader_cache::VertexElement::Unpack(packed);
    VertexInput in;
    in.location = element.location;
    in.usage = element.usage;
    in.is_uint = element.is_uint;
    ucode::VertexFetchInstruction fetch;
    if (vertex_fetch_at(element.address, fetch)) {
      // A mini fetch takes the fetch constant and stride of the previous
      // full fetch.
      ucode::VertexFetchInstruction full = fetch;
      bool have_full = !fetch.is_mini_fetch();
      for (size_t a = element.address; !have_full && a-- > 0;) {
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
      }
    }
    vertex_inputs.push_back(in);
  }
}

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
      color_rts[i] = create ? render_targets_.GetColor(cmd, colors[i].base, pitch, msaa,
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

NativeCommandProcessor::DrawResult NativeCommandProcessor::UploadGeometry(
    const MantraShader& vs, xenos::PrimitiveType prim_type, uint32_t index_count,
    IndexBufferInfo* index_buffer_info, bool restart_allowed, mantra::PipelineKey& key,
    GeometryUpload& upload, uint32_t& spec_constants) {
  const RegisterFile& regs = *register_file_;
  upload.buffer = shader_interface_.ring_buffer();
  const uint32_t index_base = regs[XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;

  // Indices: guest indices (endian-swapped) or a generated sequence when the
  // primitive type has to be converted.
  const bool quad_list = prim_type == xenos::PrimitiveType::kQuadList;
  const bool line_loop = prim_type == xenos::PrimitiveType::kLineLoop;
  const bool guest_indexed = index_buffer_info && index_buffer_info->guest_base;
  uint32_t min_index = 0, max_index = index_count ? index_count - 1 : 0;
  std::vector<uint32_t> indices;
  bool restart = false;
  if (guest_indexed) {
    const bool is_32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(
        index_buffer_info->guest_base & kGuestPhysicalMemoryMask);
    if (!src) {
      return DrawResult::kSkipped;
    }
    const auto mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    const uint32_t reset_index =
        regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx & (is_32 ? 0xFFFFFF : 0xFFFF);
    const bool use_reset = mode.multi_prim_ib_ena != 0;
    restart = use_reset && restart_allowed;
    indices.resize(index_count);
    min_index = UINT32_MAX;
    max_index = 0;
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
      min_index = std::min(min_index, v);
      max_index = std::max(max_index, v);
    }
    if (min_index > max_index) {
      return DrawResult::kSkipped;  // only reset indices
    }
    if (use_reset && !restart) {
      // No host restart for this topology: point reset indices at a valid
      // vertex instead of past the uploaded range.
      for (uint32_t& v : indices) {
        if (v == UINT32_MAX) v = min_index;
      }
    }
  } else if (quad_list || line_loop) {
    indices.resize(index_count);
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

  // Vertex buffers: one binding per fetch constant; the vertex range
  // [min_index, max_index] (+ VGT_INDX_OFFSET) is copied, endian-swapped per
  // dword with the fetch constant's endianness.
  const uint64_t first_vertex = uint64_t(min_index) + index_base;
  const uint32_t vertex_count = max_index - min_index + 1;
  uint32_t binding_fetch_constants[mantra::kMaxVertexBindings];
  uint32_t binding_count = 0;
  bool packed_normal = false;
  bool need_zero_binding = false;
  uint32_t attribute_count = 0;
  uint32_t zero_binding_attributes[mantra::kMaxVertexAttributes];
  uint32_t zero_attribute_count = 0;
  for (const MantraShader::VertexInput& in : vs.vertex_inputs) {
    if (attribute_count >= mantra::kMaxVertexAttributes) break;
    auto& attr = key.attributes[attribute_count++];
    attr.location = uint8_t(in.location);
    const VkFormat format = in.valid ? VertexAttributeFormat(in, packed_normal)
                                     : VK_FORMAT_UNDEFINED;
    const auto fetch = in.valid ? regs.GetVertexFetch(in.fetch_constant)
                                : xenos::xe_gpu_vertex_fetch_t{};
    if (format == VK_FORMAT_UNDEFINED || fetch.type != xenos::FetchConstantType::kVertex ||
        in.offset_words < 0) {
      // Fed zeros from a dedicated binding.
      ++guest_draw_stats_.missing_inputs;
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
    if (size64 > kMaxVertexUpload) {
      MANTRA_WARN_ONCE("draw with {} bytes of vertex data, skipping (no buffer cache yet)",
                       size64);
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
    // Vertex i of the draw (index i + vertex_offset) is at offset + i * stride.
    upload.binding_offsets[b] = offset;
    guest_draw_stats_.vertex_bytes += size;
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
    upload.binding_offsets[b] = offset;
    for (uint32_t i = 0; i < zero_attribute_count; ++i) {
      key.attributes[zero_binding_attributes[i]].binding = uint8_t(b);
    }
  }
  key.binding_count = uint8_t(binding_count);
  key.attribute_count = uint8_t(attribute_count);
  upload.binding_count = binding_count;

  if (!indices.empty() || guest_indexed) {
    // Host indices are relative to first_vertex... except that the ring copy
    // starts at first_vertex, so subtract min_index via vertex_offset.
    const bool host_32 = max_index > 0xFFFE || restart;
    const uint32_t index_size = host_32 ? 4 : 2;
    const uint32_t bytes = uint32_t(indices.size()) * index_size;
    uint32_t offset;
    uint8_t* dest = shader_interface_.Allocate(std::max<uint32_t>(bytes, 4), offset);
    if (!dest) {
      return DrawResult::kFailed;
    }
    if (host_32) {
      auto* out = reinterpret_cast<uint32_t*>(dest);
      for (size_t i = 0; i < indices.size(); ++i) out[i] = indices[i];
    } else {
      auto* out = reinterpret_cast<uint16_t*>(dest);
      for (size_t i = 0; i < indices.size(); ++i) out[i] = uint16_t(indices[i]);
    }
    upload.indexed = true;
    upload.index_offset = offset;
    upload.index_type = host_32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    upload.draw_count = uint32_t(indices.size());
    upload.vertex_offset = -int32_t(min_index);
    guest_draw_stats_.index_bytes += bytes;
  } else {
    upload.indexed = false;
    upload.draw_count = index_count;
  }
  key.primitive_restart = restart ? 1 : 0;
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
  mantra::SharedConstants shared;
  std::copy_n(viewport.ndc_scale, 3, shared.ndc_scale);
  std::copy_n(viewport.ndc_offset, 3, shared.ndc_offset);
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
    if (!PrepareDrawConstants(offsets, shared)) {
      return DrawResult::kFailed;
    }
    if (submission_current_ == submission) {
      key.spec_constants = attempt_spec;
      uploaded = true;
    }
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
  }

  const VkPipeline pipeline = draw_pipelines_.Get(key);
  if (!pipeline) {
    ++guest_draw_stats_.failed_pipeline;
    return DrawResult::kFailed;
  }

  const auto& dfn = vulkan_device_->functions();
  dfn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  shader_interface_.Bind(cmd, offsets);

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
  const float blend_constants[4] = {
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

  // Geometry.
  if (upload.binding_count) {
    VkBuffer buffers[mantra::kMaxVertexBindings];
    std::fill_n(buffers, upload.binding_count, upload.buffer);
    dfn.vkCmdBindVertexBuffers(cmd, 0, upload.binding_count, buffers, upload.binding_offsets);
  }
  if (upload.indexed) {
    dfn.vkCmdBindIndexBuffer(cmd, upload.buffer, upload.index_offset, upload.index_type);
    dfn.vkCmdDrawIndexed(cmd, upload.draw_count, 1, 0, upload.vertex_offset, 0);
  } else {
    dfn.vkCmdDraw(cmd, upload.draw_count, 1, 0, 0);
  }
  ++guest_draw_stats_.drawn;
  return DrawResult::kDrawn;
}

}  // namespace rex::graphics::native
