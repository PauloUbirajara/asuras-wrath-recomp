#include "memexport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/registers.h>
#include <rex/memory.h>
#include <rex/system/xmemory.h>

namespace mantra {

namespace {

namespace xenos = rex::graphics::xenos;
namespace ucode = rex::graphics::ucode;
namespace reg = rex::graphics::reg;
using rex::graphics::ShaderInterpreter;

uint32_t FloatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

// Float -> fixed point by the Direct3D rules (Xenia's memexport packing).
uint32_t ToFixed(float v, uint32_t width, bool is_signed, bool is_norm) {
  if (std::isnan(v)) v = 0.0f;
  if (is_signed) {
    const float max = float((1u << (width - 1)) - 1);
    if (is_norm) v = std::clamp(v, -1.0f, 1.0f) * max;
    v += v < 0.0f ? -0.5f : 0.5f;
    const float lo = -float(1u << (width - 1));
    return uint32_t(int32_t(std::clamp(v, lo, max)));
  }
  const float max = float((width >= 32 ? 0xFFFFFFFFu : (1u << width) - 1));
  if (is_norm) v = std::clamp(v, 0.0f, 1.0f) * max;
  v += 0.5f;
  return uint32_t(std::clamp(v, 0.0f, max));
}

uint16_t ToHalf(float f) {
  // IEEE float -> half, round to nearest even (as packHalf2x16).
  uint32_t x = FloatBits(f);
  const uint32_t sign = (x >> 16) & 0x8000;
  x &= 0x7FFFFFFF;
  if (x >= 0x7F800000) return uint16_t(sign | (x > 0x7F800000 ? 0x7E00 : 0x7C00));
  if (x < 0x38800000) {  // half denormal or zero
    if (x < 0x33000000) return uint16_t(sign);
    const uint32_t shift = 126 - (x >> 23);
    const uint32_t m = (x & 0x7FFFFF) | 0x800000;
    uint32_t r = m >> shift;
    const uint32_t rem = m & ((1u << shift) - 1), halfway = 1u << (shift - 1);
    if (rem > halfway || (rem == halfway && (r & 1))) ++r;
    return uint16_t(sign | r);
  }
  x -= 0x38000000;
  x += 0xFFF + ((x >> 13) & 1);
  if (x >= (0x7C00u << 13)) return uint16_t(sign | 0x7C00);
  return uint16_t(sign | (x >> 13));
}

uint32_t Swap32(uint32_t v, xenos::Endian128 endian) {
  switch (endian) {
    case xenos::Endian128::k8in16:
      return ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
    case xenos::Endian128::k8in32:
      return __builtin_bswap32(v);
    case xenos::Endian128::k16in32:
      return (v << 16) | (v >> 16);
    default:
      return v;
  }
}

class Sink : public ShaderInterpreter::ExportSink {
 public:
  Sink(rex::memory::Memory& memory, MemExportStats& stats, MemExportWritten written,
       void* context)
      : memory_(memory), stats_(stats), written_(written), context_(context) {}

  void AllocExport(ucode::AllocType, uint32_t) override { Flush(); }
  void Export(ucode::ExportRegister reg, const float* value, uint32_t mask) override {
    const uint32_t r = uint32_t(reg);
    if (r == uint32_t(ucode::ExportRegister::kExportAddress)) {
      Flush();
      for (int i = 0; i < 4; ++i) {
        if (mask & (1u << i)) eA_[i] = FloatBits(value[i]);
      }
      has_eA_ = true;
    } else if (r >= uint32_t(ucode::ExportRegister::kExportData0) &&
               r < uint32_t(ucode::ExportRegister::kExportData0) + ucode::kMaxMemExportElementCount) {
      const uint32_t i = r - uint32_t(ucode::ExportRegister::kExportData0);
      std::memcpy(eM_[i], value, sizeof(eM_[i]));
      written_mask_ |= uint8_t(1u << i);
    }
  }
  void Flush();
  bool bad_format = false;

 private:
  rex::memory::Memory& memory_;
  MemExportStats& stats_;
  MemExportWritten written_;
  void* context_;
  uint32_t eA_[4] = {};
  bool has_eA_ = false;
  float eM_[ucode::kMaxMemExportElementCount][4] = {};
  uint8_t written_mask_ = 0;
};

void Sink::Flush() {
  const uint8_t mask = written_mask_;
  written_mask_ = 0;
  if (!mask || !has_eA_) return;
  // The address must have the stream constant's sign/exponent markers.
  if ((eA_[0] >> 30) != 0x1 || (eA_[1] >> 23) != 0x96 || (eA_[2] >> 23) != 0x96 ||
      (eA_[3] >> 23) != 0x96) {
    return;
  }
  const uint32_t info = eA_[2];
  const auto endian = xenos::Endian128(info & 7);
  const auto format = xenos::ColorFormat((info >> 8) & 63);
  const bool is_signed = (info >> 16) & 1;
  const bool is_norm = !((info >> 17) & 1);
  const bool swap_rb = (info >> 19) & 1;
  const uint32_t eM0_index = eA_[1] & 0x7FFFFF;
  const uint32_t index_count = eA_[3] & 0x7FFFFF;
  const uint32_t base = eA_[0] << 2;

  for (uint32_t e = 0; e < ucode::kMaxMemExportElementCount; ++e) {
    if (!(mask & (1u << e))) continue;
    const uint32_t index = eM0_index + e;
    if (index >= index_count) continue;
    float v[4];
    std::memcpy(v, eM_[e], sizeof(v));
    if (swap_rb) std::swap(v[0], v[2]);
    uint32_t packed[4] = {};
    uint32_t size_log2 = 2;
    auto pack = [&](std::initializer_list<uint32_t> widths) {
      uint32_t offset = 0, c = 0;
      for (uint32_t w : widths) {
        packed[0] |= (ToFixed(v[c++], w, is_signed, is_norm) & ((w >= 32) ? ~0u : (1u << w) - 1))
                     << offset;
        offset += w;
      }
      size_log2 = offset == 8 ? 0 : offset == 16 ? 1 : 2;
    };
    switch (format) {
      case xenos::ColorFormat::k_8:
      case xenos::ColorFormat::k_8_A:
      case xenos::ColorFormat::k_8_B: pack({8}); break;
      case xenos::ColorFormat::k_1_5_5_5: pack({5, 5, 5, 1}); break;
      case xenos::ColorFormat::k_5_6_5: pack({5, 6, 5}); break;
      case xenos::ColorFormat::k_6_5_5: pack({5, 5, 6}); break;
      case xenos::ColorFormat::k_8_8_8_8:
      case xenos::ColorFormat::k_8_8_8_8_A:
      case xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16: pack({8, 8, 8, 8}); break;
      case xenos::ColorFormat::k_2_10_10_10:
      case xenos::ColorFormat::k_2_10_10_10_AS_16_16_16_16: pack({10, 10, 10, 2}); break;
      case xenos::ColorFormat::k_8_8: pack({8, 8}); break;
      case xenos::ColorFormat::k_4_4_4_4: pack({4, 4, 4, 4}); break;
      case xenos::ColorFormat::k_10_11_11:
      case xenos::ColorFormat::k_10_11_11_AS_16_16_16_16: pack({11, 11, 10}); break;
      case xenos::ColorFormat::k_11_11_10:
      case xenos::ColorFormat::k_11_11_10_AS_16_16_16_16: pack({10, 11, 11}); break;
      case xenos::ColorFormat::k_16: pack({16}); break;
      case xenos::ColorFormat::k_16_16: pack({16, 16}); break;
      case xenos::ColorFormat::k_16_16_16_16:
        for (int c = 0; c < 4; ++c) {
          packed[c >> 1] |= (ToFixed(v[c], 16, is_signed, is_norm) & 0xFFFF) << (16 * (c & 1));
        }
        size_log2 = 3;
        break;
      case xenos::ColorFormat::k_16_FLOAT:
        packed[0] = ToHalf(v[0]);
        size_log2 = 1;
        break;
      case xenos::ColorFormat::k_16_16_FLOAT:
        packed[0] = ToHalf(v[0]) | (uint32_t(ToHalf(v[1])) << 16);
        break;
      case xenos::ColorFormat::k_16_16_16_16_FLOAT:
        packed[0] = ToHalf(v[0]) | (uint32_t(ToHalf(v[1])) << 16);
        packed[1] = ToHalf(v[2]) | (uint32_t(ToHalf(v[3])) << 16);
        size_log2 = 3;
        break;
      case xenos::ColorFormat::k_32_FLOAT:
        packed[0] = FloatBits(v[0]);
        break;
      case xenos::ColorFormat::k_32_32_FLOAT:
        packed[0] = FloatBits(v[0]);
        packed[1] = FloatBits(v[1]);
        size_log2 = 3;
        break;
      case xenos::ColorFormat::k_32_32_32_32_FLOAT:
        for (int c = 0; c < 4; ++c) packed[c] = FloatBits(v[c]);
        size_log2 = 4;
        break;
      default:
        bad_format = true;
        return;
    }
    // 128-bit endian swap: 8-in-64 / 8-in-128 swap dwords, then 8-in-32.
    xenos::Endian128 e32 = endian;
    if (endian == xenos::Endian128::k8in64) {
      std::swap(packed[0], packed[1]);
      std::swap(packed[2], packed[3]);
      e32 = xenos::Endian128::k8in32;
    } else if (endian == xenos::Endian128::k8in128) {
      std::swap(packed[0], packed[3]);
      std::swap(packed[1], packed[2]);
      e32 = xenos::Endian128::k8in32;
    }
    for (uint32_t& p : packed) p = Swap32(p, e32);

    const uint32_t size = 1u << size_log2;
    const uint32_t address = (base + (index << size_log2)) & 0x1FFFFFFF;
    auto* dest = memory_.TranslatePhysical<uint8_t*>(address);
    if (!dest) continue;
    // Host little-endian dwords, as the GPU's stores; a sub-dword element
    // is in the low bits of packed[0].
    std::memcpy(dest, packed, size);
    written_(context_, address, size);
    ++stats_.elements;
    stats_.bytes += size;
  }
}

}  // namespace

bool RunMemExportDraw(const rex::graphics::RegisterFile& regs, rex::memory::Memory& memory,
                      const rex::graphics::Shader& vs, uint32_t index_count,
                      const MemExportIndices* index_buffer,
                      MemExportStats& stats, MemExportWritten written, void* context) {
  if (!ShaderInterpreter::CanInterpretShader(vs)) {
    ++stats.unsupported;
    return false;
  }
  ShaderInterpreter interpreter(regs, memory);
  interpreter.SetShader(vs);
  Sink sink(memory, stats, written, context);
  interpreter.SetExportSink(&sink);

  const uint32_t index_offset = regs[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET];
  const uint32_t min_index = regs[rex::graphics::XE_GPU_REG_VGT_MIN_VTX_INDX];
  const uint32_t max_index = regs[rex::graphics::XE_GPU_REG_VGT_MAX_VTX_INDX];
  const uint8_t* indices = nullptr;
  if (index_buffer) {
    indices = memory.TranslatePhysical<const uint8_t*>(index_buffer->guest_base);
    index_count = std::min<uint32_t>(index_count, index_buffer->count);
  }
  for (uint32_t i = 0; i < index_count; ++i) {
    uint32_t vertex_index = i;
    if (indices) {
      if (!index_buffer->is_32bit) {
        uint16_t v;
        std::memcpy(&v, indices + 2 * i, 2);
        vertex_index = xenos::GpuSwap(uint32_t(v), index_buffer->endianness);
      } else {
        uint32_t v;
        std::memcpy(&v, indices + 4 * i, 4);
        vertex_index = xenos::GpuSwap(v, index_buffer->endianness);
      }
      vertex_index &= 0xFFFFFF;
    }
    vertex_index =
        std::min(max_index, std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));
    float* r = interpreter.temp_registers();
    r[0] = float(vertex_index);
    r[1] = r[2] = r[3] = 0.0f;
    interpreter.Execute();
    sink.Flush();
    ++stats.vertices;
  }
  if (sink.bad_format) ++stats.unsupported;
  ++stats.draws;
  return true;
}

}  // namespace mantra
