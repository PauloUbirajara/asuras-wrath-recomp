#pragma once
// CPU execution of memexport vertex shaders.
//
// Xenos vertex shaders can write straight to memory (eA = stream address,
// eM0..eM4 = elements). Games use VS-only point-list draws with no color
// writes as a GPU memcpy, e.g. UE3's texture pool moving textures while
// defragmenting. The translated host shaders have no memory export, so such
// draws are run here, one vertex at a time, with the SDK's ucode interpreter,
// and the elements are packed into guest memory the way Xenia's translators
// do it (spirv_translator_memexport.cpp).
#include <cstdint>
#include <type_traits>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>

namespace rex::memory {
class Memory;
}

namespace mantra {

// The draw's index buffer (CommandProcessor::IndexBufferInfo is protected).
struct MemExportIndices {
  uint32_t guest_base = 0;
  uint32_t count = 0;
  bool is_32bit = false;
  rex::graphics::xenos::Endian endianness = rex::graphics::xenos::Endian::kNone;
};

struct MemExportStats {
  uint64_t draws = 0;
  uint64_t vertices = 0;
  uint64_t elements = 0;
  uint64_t bytes = 0;
  uint64_t unsupported = 0;  // draws not run (texture fetches, bad format...)
};

// Runs the vertex shader of the current draw for each of its vertices and
// performs its memory exports. Calls `written(start, length)` for every
// contiguous physical range it wrote. Returns false if the shader can't be
// run on the CPU.
template <typename F>
bool RunMemExportDraw(const rex::graphics::RegisterFile& regs, rex::memory::Memory& memory,
                      const rex::graphics::Shader& vs, uint32_t index_count,
                      const MemExportIndices* index_buffer,
                      MemExportStats& stats, F&& written);

// Non-template core (written ranges reported through a function pointer).
using MemExportWritten = void (*)(void* context, uint32_t start, uint32_t length);
bool RunMemExportDraw(const rex::graphics::RegisterFile& regs, rex::memory::Memory& memory,
                      const rex::graphics::Shader& vs, uint32_t index_count,
                      const MemExportIndices* index_buffer,
                      MemExportStats& stats, MemExportWritten written, void* context);

template <typename F>
bool RunMemExportDraw(const rex::graphics::RegisterFile& regs, rex::memory::Memory& memory,
                      const rex::graphics::Shader& vs, uint32_t index_count,
                      const MemExportIndices* index_buffer,
                      MemExportStats& stats, F&& written) {
  return RunMemExportDraw(
      regs, memory, vs, index_count, index_buffer, stats,
      [](void* context, uint32_t start, uint32_t length) {
        (*static_cast<std::remove_reference_t<F>*>(context))(start, length);
      },
      const_cast<void*>(static_cast<const void*>(&written)));
}

}  // namespace mantra
