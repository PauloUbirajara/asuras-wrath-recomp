// Interim draw path: the D3D driver's solid-fill program.
//
// D3D's Clear (and some letterbox/fill rects) is a 3-vertex rectangle-list
// draw with two inline programs: a 27-dword VS passing position and a
// per-vertex float4 color through, and the 9-dword PS `oC0 = interpolator0`.
// With no blending and depth test ALWAYS this is exactly a clear of the
// covered rectangle, so it is executed as one on the host render targets
// until the real draw path exists.
#include "mantra_log.h"
#include "native_command_processor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/system/xmemory.h>
#include <xxhash.h>

namespace rex::graphics::native {

namespace {

constexpr uint64_t kSolidFillVs = 0x1E6883FCCDE1F688;
constexpr uint64_t kSolidFillPs = 0xA4A965C189287B99;

uint32_t VertexFormatComponents(xenos::VertexFormat format) {
  switch (format) {
    case xenos::VertexFormat::k_32_FLOAT: return 1;
    case xenos::VertexFormat::k_32_32_FLOAT: return 2;
    case xenos::VertexFormat::k_32_32_32_FLOAT: return 3;
    case xenos::VertexFormat::k_32_32_32_32_FLOAT: return 4;
    default: return 0;
  }
}

}  // namespace

NativeCommandProcessor::SolidFillResult NativeCommandProcessor::TrySolidFill(
    xenos::PrimitiveType prim_type, xenos::EdramMode edram_mode) {
  Shader* vs = active_vertex_shader();
  Shader* ps = active_pixel_shader();
  if (!vs || !ps || vs->ucode_data_hash() != kSolidFillVs ||
      ps->ucode_data_hash() != kSolidFillPs) {
    return SolidFillResult::kNotSolidFill;
  }
  if (prim_type != xenos::PrimitiveType::kRectangleList) {
    return SolidFillResult::kNotSolidFill;
  }
  const RegisterFile& regs = *register_file_;

  // Vertices: position and color, both float, in one binding.
  const auto& bindings = vs->vertex_bindings();
  if (bindings.size() != 1 || bindings[0].attributes.size() < 2) {
    MANTRA_WARN_ONCE("solid fill: unexpected vertex layout ({} bindings)", bindings.size());
    return SolidFillResult::kUnsupported;
  }
  const auto& binding = bindings[0];
  const auto& pos_attr = binding.attributes[0].fetch_instr.attributes;
  const auto& color_attr = binding.attributes[1].fetch_instr.attributes;
  const uint32_t pos_count = VertexFormatComponents(pos_attr.data_format);
  const uint32_t color_count = VertexFormatComponents(color_attr.data_format);
  const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(binding.fetch_constant);
  const uint32_t stride = binding.stride_words;
  if (pos_count < 2 || color_count != 4 || !stride ||
      fetch.type != xenos::FetchConstantType::kVertex) {
    MANTRA_WARN_ONCE("solid fill: unexpected vertex formats {} / {}", uint32_t(pos_attr.data_format),
                     uint32_t(color_attr.data_format));
    return SolidFillResult::kUnsupported;
  }
  const uint32_t* vertex_words =
      reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(fetch.address << 2));
  if (!vertex_words || (stride * 3) > fetch.size) {
    return SolidFillResult::kUnsupported;
  }
  auto read_float = [&](uint32_t vertex, int32_t offset, uint32_t component) {
    uint32_t bits = xenos::GpuSwap(vertex_words[vertex * stride + offset + component],
                                   fetch.endian);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  };

  // Viewport transform (the VS passes the position through).
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto reg_float = [&](uint32_t index) { return regs.Get<float>(index); };
  const float x_scale = vte.vport_x_scale_ena ? reg_float(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
  const float x_offset = vte.vport_x_offset_ena ? reg_float(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f;
  const float y_scale = vte.vport_y_scale_ena ? reg_float(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
  const float y_offset = vte.vport_y_offset_ena ? reg_float(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
  const float z_scale = vte.vport_z_scale_ena ? reg_float(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
  const float z_offset = vte.vport_z_offset_ena ? reg_float(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
  float x_min = INFINITY, y_min = INFINITY, x_max = -INFINITY, y_max = -INFINITY;
  for (uint32_t v = 0; v < 3; ++v) {
    const float x = read_float(v, pos_attr.offset, 0) * x_scale + x_offset;
    const float y = read_float(v, pos_attr.offset, 1) * y_scale + y_offset;
    x_min = std::min(x_min, x);
    x_max = std::max(x_max, x);
    y_min = std::min(y_min, y);
    y_max = std::max(y_max, y);
  }
  const float z = pos_count >= 3 ? read_float(0, pos_attr.offset, 2) * z_scale + z_offset : 0.0f;
  float color[4];
  for (uint32_t c = 0; c < 4; ++c) {
    color[c] = read_float(0, color_attr.offset, c);
  }

  // Pixel coverage (pixel centers at .5, or at .0 with the D3D9 convention).
  const float half_pixel =
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  int32_t x0 = int32_t(std::lround(x_min + half_pixel));
  int32_t y0 = int32_t(std::lround(y_min + half_pixel));
  int32_t x1 = int32_t(std::lround(x_max + half_pixel));
  int32_t y1 = int32_t(std::lround(y_max + half_pixel));
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    const auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    x0 += window_offset.window_x_offset;
    x1 += window_offset.window_x_offset;
    y0 += window_offset.window_y_offset;
    y1 += window_offset.window_y_offset;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, true);
  x0 = std::max(x0, int32_t(scissor.offset[0]));
  y0 = std::max(y0, int32_t(scissor.offset[1]));
  x1 = std::min(x1, int32_t(scissor.offset[0] + scissor.extent[0]));
  y1 = std::min(y1, int32_t(scissor.offset[1] + scissor.extent[1]));
  if (x1 <= x0 || y1 <= y0) {
    return SolidFillResult::kDone;
  }
  const VkRect2D rect = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};

  // Only a plain overwrite is a clear.
  const auto depth_control = regs.Get<reg::RB_DEPTHCONTROL>();
  const bool depth_test = depth_control.z_enable && depth_control.zfunc != xenos::CompareFunction::kAlways;
  const bool stencil_test =
      depth_control.stencil_enable && depth_control.stencilfunc != xenos::CompareFunction::kAlways;
  if (depth_test || stencil_test) {
    MANTRA_WARN_ONCE("solid fill with depth/stencil testing, skipping");
    return SolidFillResult::kUnsupported;
  }

  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  VkCommandBuffer cmd = BeginNonDrawCommands();
  if (!cmd) {
    return SolidFillResult::kUnsupported;
  }
  const uint64_t frame = submission_current_;

  bool wrote = false;
  const uint32_t color_mask = regs.Get<reg::RB_COLOR_MASK>().value & 0xF;
  if (edram_mode == xenos::EdramMode::kColorDepth && color_mask) {
    const auto blend = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]);
    const bool overwrite = blend.color_srcblend == xenos::BlendFactor::kOne &&
                           blend.color_destblend == xenos::BlendFactor::kZero &&
                           blend.color_comb_fcn == xenos::BlendOp::kAdd &&
                           blend.alpha_srcblend == xenos::BlendFactor::kOne &&
                           blend.alpha_destblend == xenos::BlendFactor::kZero &&
                           blend.alpha_comb_fcn == xenos::BlendOp::kAdd;
    if (!overwrite || color_mask != 0xF) {
      MANTRA_WARN_ONCE("solid fill with blending or a partial color mask (0x{:X}), skipping",
                       color_mask);
      ++draw_stats_.fills_skipped;
      return SolidFillResult::kUnsupported;
    }
    const auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
    if (mantra::RenderTarget* rt =
            GetColorTarget(cmd, color_info.color_base, surface_info.surface_pitch,
                                     surface_info.msaa_samples, color_info.color_format, frame)) {
      const float bias = std::ldexp(1.0f, color_info.color_exp_bias);
      float value[4];
      for (uint32_t c = 0; c < 4; ++c) {
        value[c] = color[c] * bias;
      }
      // MANTRA_DEBUG_FILL_TINT=1: paint each fill with a color derived from
      // its target and rectangle, to make the fill -> resolve -> present
      // chain visible (the game's own clears are mostly black).
      static const bool tint = std::getenv("MANTRA_DEBUG_FILL_TINT") != nullptr;
      if (tint) {
        const uint64_t h = XXH3_64bits(&rect, sizeof(rect)) ^ color_info.color_base;
        for (uint32_t c = 0; c < 3; ++c) {
          value[c] = 0.25f + 0.75f * float((h >> (c * 8)) & 0xFF) / 255.0f;
        }
        value[3] = 1.0f;
      }
      if (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
          color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
        for (float& v : value) v *= 1.0f / 32.0f;  // -32...32 in the host SNORM image
      }
      if (color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        for (uint32_t c = 0; c < 3; ++c) {
          value[c] = xenos::LinearToPWLGamma(std::clamp(value[c], 0.0f, 1.0f));
        }
      }
      render_targets_.ClearColorFloat(cmd, *rt, color_info.color_format, rect, value);
      for (const auto& alias : render_targets_.Aliases(*rt, rect)) {
        render_targets_.ClearColorFloat(cmd, *alias.rt, color_info.color_format, alias.rect,
                                        value);
        render_targets_.MarkWritten(*alias.rt, color_info.color_format);
      }
      uint64_t route = XXH3_64bits(value, sizeof(value)) ^ (uint64_t(color_info.color_base) << 1) ^
                       (uint64_t(surface_info.surface_pitch) << 13) ^
                       (uint64_t(color_info.color_format) << 29);
      if (FirstRoute(route)) {
        MANTRA_INFO("fill: rt base {} pitch {} fmt {} rect {},{} {}x{} color {:.3f} {:.3f} {:.3f} {:.3f}",
                    uint32_t(color_info.color_base), uint32_t(surface_info.surface_pitch),
                    uint32_t(color_info.color_format), x0, y0, x1 - x0, y1 - y0, value[0],
                    value[1], value[2], value[3]);
      }
      ++draw_stats_.fills_color;
      wrote = true;
    }
  }

  const bool write_depth = depth_control.z_enable && depth_control.z_write_enable;
  const bool write_stencil = depth_control.stencil_enable &&
                             depth_control.stencilzpass == xenos::StencilOp::kReplace &&
                             regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask == 0xFF;
  if (write_depth || write_stencil) {
    const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    if (mantra::RenderTarget* rt =
            render_targets_.GetDepth(cmd, depth_info.depth_base, surface_info.surface_pitch,
                                     surface_info.msaa_samples, frame)) {
      float depth = std::clamp(z, 0.0f, 1.0f);
      if (depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
        depth *= 0.5f;  // [0, 2) guest range in [0, 1) host depth
      }
      const uint32_t stencil_ref = regs.Get<reg::RB_STENCILREFMASK>().stencilref;
      render_targets_.ClearDepthStencil(cmd, *rt, rect, write_depth, depth, write_stencil,
                                        stencil_ref);
      for (const auto& alias : render_targets_.Aliases(*rt, rect)) {
        render_targets_.ClearDepthStencil(cmd, *alias.rt, alias.rect, write_depth, depth,
                                          write_stencil, stencil_ref);
      }
      // Depth and 32bpp color share EDRAM: a depth clear also overwrites the
      // color data in the same tiles. Games use that to fill color fast
      // (Asura's Wrath clears its shadow-attenuation buffer to white through
      // depth 1.0 / stencil 0xFF at the color buffer's base). Only full-word
      // clears, whose EDRAM value is known: (depth24 << 8) | stencil; and only
      // into a 32bpp target that already is the newest copy of its EDRAM:
      // clearing one whose 7e3 (64bpp) counterpart is newer would make the
      // whole stale image the newest, and the next 7e3 draw would convert
      // all of it, losing what the clear didn't cover.
      if (write_depth && write_stencil) {
        const float guest_depth = std::clamp(z, 0.0f, 1.0f);
        const uint32_t depth24 =
            depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8
                ? xenos::Float32To20e4(guest_depth, true)
                : uint32_t(std::lround(double(guest_depth) * 0xFFFFFF));
        const uint64_t word = (uint64_t(depth24 & 0xFFFFFF) << 8) | (stencil_ref & 0xFF);
        for (const auto& alias : render_targets_.Aliases(*rt, rect, mantra::RtClass::kColor32)) {
          if (render_targets_.CounterpartIsNewer(*alias.rt)) {
            continue;
          }
          render_targets_.ClearColor(cmd, *alias.rt, alias.rt->last_format, alias.rect, word);
          render_targets_.MarkWritten(*alias.rt, alias.rt->last_format);
          if (TracingThisFrame()) {
            MANTRA_INFO("trace: fill depth -> color alias base {} pitch {} fmt {} rect {},{} {}x{} "
                        "word {:08X}",
                        alias.rt->key.base_tiles, alias.rt->key.pitch_px,
                        uint32_t(alias.rt->last_format), alias.rect.offset.x, alias.rect.offset.y,
                        alias.rect.extent.width, alias.rect.extent.height, uint32_t(word));
          }
        }
      }
      static std::atomic<uint32_t> logged{0};
      if (logged.fetch_add(1) < 8 || TracingThisFrame()) {
        MANTRA_INFO("fill depth: base {} pitch {} msaa {} rect {},{} {}x{} depth {} stencil {} "
                    "({}) aliases {}",
                    uint32_t(depth_info.depth_base), uint32_t(surface_info.surface_pitch),
                    uint32_t(surface_info.msaa_samples), x0, y0, x1 - x0, y1 - y0,
                    write_depth, write_stencil, stencil_ref,
                    render_targets_.Aliases(*rt, rect).size());
      }
      ++draw_stats_.fills_depth;
      wrote = true;
    }
  }
  if (!wrote) {
    ++draw_stats_.fills_skipped;
  }
  return SolidFillResult::kDone;
}

}  // namespace rex::graphics::native
