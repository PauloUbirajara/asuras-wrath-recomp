#pragma once
// Shader identity keys shared by mantra (runtime lookup) and mantra_shadergen
// (offline translation). Input is Xenos microcode exactly as the game uploads
// it: big-endian dwords.
//
// - Exact key: XXH3 over the raw bytes. Pixel shaders match the game files
//   this way.
// - Normalized key: the D3D driver patches vertex shaders at load time to fit
//   the bound vertex declaration and pixel shader, so their bytes differ from
//   the shader containers in the game files. The normalized key masks what the
//   driver changes:
//     * vertex fetch: fetch constant + prefetch (word 0 bits 20-29), all of
//       word 1 except the predicate bit (format, signedness, destination
//       swizzle, mini-fetch), and word 2 (stride, offset); and the order of
//       the vertex fetches (the driver reorders them, even across clauses,
//       which permutes their destination registers), by sorting them;
//     * ALU instructions that export or write nothing (the driver turns
//       exports of unused interpolators into writes with empty masks).
//   The vertex input layout always comes from the runtime microcode, so the
//   masked fields are recovered at pipeline creation.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <xxhash.h>

#include <rex/graphics/format/ucode.h>

namespace mantra::shader_key {

inline uint64_t ExactHash(const void* microcode_be, size_t byte_count) {
  return XXH3_64bits(microcode_be, byte_count);
}

inline uint64_t NormalizedHash(const void* microcode_be, size_t byte_count) {
  namespace ucode = rex::graphics::ucode;
  const size_t dword_count = byte_count / sizeof(uint32_t);
  std::vector<uint32_t> code(dword_count);
  std::memcpy(code.data(), microcode_be, dword_count * sizeof(uint32_t));
  for (uint32_t& dword : code) {
    dword = __builtin_bswap32(dword);
  }

  // Walk the control flow program (pairs of CF instructions per 3 dwords) up
  // to the first instruction address, collecting exec clauses.
  struct Clause {
    uint32_t address, count, sequence;
  };
  std::vector<Clause> clauses;
  size_t first_instruction = dword_count / 3;
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
          // All exec variants share the address/count/sequence layout.
          const auto& exec = instr.exec;
          if (exec.count()) {
            clauses.push_back({exec.address(), exec.count(), exec.sequence()});
            first_instruction = std::min<size_t>(first_instruction, exec.address());
          }
          break;
        }
        default:
          break;
      }
    }
  }

  // Vertex fetches, sorted back into the program's vertex fetch slots (other
  // fetches and ALU instructions stay where they are). The driver moves them
  // across clauses too.
  struct Fetch {
    uint32_t w[3];
    bool operator<(const Fetch& o) const { return std::memcmp(w, o.w, sizeof(w)) < 0; }
  };
  std::vector<Fetch> fetches;
  std::vector<size_t> slots;
  for (const Clause& clause : clauses) {
    for (uint32_t i = 0; i < clause.count; ++i) {
      const size_t base = size_t(clause.address + i) * 3;
      if (base + 2 >= dword_count) {
        break;
      }
      uint32_t* w = &code[base];
      const bool is_fetch = (clause.sequence >> (i * 2)) & 1;
      if (is_fetch) {
        if ((w[0] & 0x1F) == uint32_t(ucode::FetchOpcode::kVertexFetch)) {
          w[0] &= ~(0x3FFu << 20);
          w[1] &= 0x80000000u;
          w[2] = 0;
          fetches.push_back({{w[0], w[1], w[2]}});
          slots.push_back(base);
        }
      } else {
        ucode::AluInstruction alu;
        std::memcpy(&alu, w, sizeof(alu));
        if (alu.is_export() || (alu.vector_write_mask() == 0 && alu.scalar_write_mask() == 0)) {
          w[0] = w[1] = w[2] = 0;
        }
      }
    }
  }
  std::sort(fetches.begin(), fetches.end());
  std::sort(slots.begin(), slots.end());
  for (size_t k = 0; k < fetches.size(); ++k) {
    std::memcpy(&code[slots[k]], fetches[k].w, sizeof(fetches[k].w));
  }
  return XXH3_64bits(code.data(), code.size() * sizeof(uint32_t));
}

// Interpolator exports of a vertex shader, one word per exporting ALU
// result: bits 0..3 interpolator register, bit 4 scalar (else vector) result,
// bits 8..19 instruction address. Only exports to o0..o15 with a non-empty
// write mask count.
//
// The normalized key ignores exports, and the driver patches a vertex
// shader's export registers to where the bound pixel shader reads each
// semantic; mantra's translations put interpolator oN at location N, so a
// normalized match must have its output locations renumbered by comparing
// these lists (see RemapVertexOutputLocations).
inline std::vector<uint32_t> InterpolatorExports(const void* microcode_be, size_t byte_count) {
  const size_t dwords = byte_count / sizeof(uint32_t);
  const auto* bytes = static_cast<const uint8_t*>(microcode_be);
  auto word = [&](size_t i) {
    const uint8_t* w = bytes + i * 4;
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
  std::vector<uint32_t> exports;
  for (size_t i = first; i < instructions; ++i) {
    if (!is_alu[i]) continue;
    const uint32_t w0 = word(i * 3);
    if (!((w0 >> 15) & 1)) continue;  // not an export
    const uint32_t vector_dest = w0 & 0x3F, scalar_dest = (w0 >> 8) & 0x3F;
    if (((w0 >> 16) & 0xF) && vector_dest < 16) {
      exports.push_back(uint32_t(i) << 8 | vector_dest);
    }
    if (((w0 >> 20) & 0xF) && scalar_dest < 16) {
      exports.push_back(uint32_t(i) << 8 | 1u << 4 | scalar_dest);
    }
  }
  return exports;
}

// Output location permutation for a vertex shader translated from microcode
// with `translated` exports and run as microcode with `runtime` exports:
// `map[t]` is the location for the output the translation put at t. Returns
// false when the exports don't correspond (then nothing should be remapped)
// or when the map is the identity.
inline bool InterpolatorLocationMap(const std::vector<uint32_t>& translated,
                                    const std::vector<uint32_t>& runtime, uint8_t (&map)[16]) {
  int8_t target[16];
  std::fill(std::begin(target), std::end(target), int8_t(-1));
  bool used[16] = {};
  for (const uint32_t t : translated) {
    // The runtime export of the same instruction result.
    const auto it = std::find_if(runtime.begin(), runtime.end(),
                                 [&](uint32_t r) { return (r & ~0xFu) == (t & ~0xFu); });
    if (it == runtime.end()) continue;  // dropped by the driver (unused by the PS)
    const uint32_t from = t & 0xF, to = *it & 0xF;
    if (target[from] >= 0 && target[from] != int8_t(to)) return false;  // inconsistent
    if (target[from] < 0 && used[to]) return false;  // two outputs onto one location
    target[from] = int8_t(to);
    used[to] = true;
  }
  // Outputs the runtime code doesn't export go to the free locations.
  uint32_t next_free = 0;
  bool identity = true;
  for (uint32_t from = 0; from < 16; ++from) {
    if (target[from] < 0) {
      while (used[next_free]) ++next_free;
      target[from] = int8_t(next_free);
      used[next_free] = true;
    }
    map[from] = uint8_t(target[from]);
    identity = identity && map[from] == from;
  }
  return !identity;
}

}  // namespace mantra::shader_key
