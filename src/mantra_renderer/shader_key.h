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
//     * vertex fetch: stream select + prefetch (word 0 bits 25-29), all of
//       word 1 except the predicate bit (format, signedness, destination
//       swizzle, mini-fetch), and word 2 (stride, offset);
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
          w[0] &= ~(0x1Fu << 25);
          w[1] &= 0x80000000u;
          w[2] = 0;
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
  return XXH3_64bits(code.data(), code.size() * sizeof(uint32_t));
}

}  // namespace mantra::shader_key
