#pragma once
// Pipelines seen in earlier runs, for building them again at startup (in the
// background) instead of in the middle of gameplay.
//
// One append-only file per GPU model: the shaders' raw (big-endian) microcode
// as the game uploads it, and each pipeline's key with the shader module
// handles replaced by the shaders' microcode hashes. At startup the shaders go
// through LoadShader (so they get the same modules and remaps as at runtime)
// and the keys are rebuilt from them.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <unordered_set>
#include <vector>

#include "draw_pipelines.h"

namespace mantra {

struct StoredShader {
  bool is_pixel = false;
  uint64_t hash = 0;            // XXH3 of the raw microcode bytes
  std::vector<uint32_t> ucode;  // raw, big-endian dwords
};

struct StoredPipeline {
  uint64_t vs = 0, ps = 0;  // microcode hashes (ps 0 = none)
  PipelineKey key;          // vertex_module / pixel_module zero
};

class PipelineStore {
 public:
  ~PipelineStore() { Close(); }

  // Reads `path` (a missing or invalid file reads as empty), rewrites it
  // compacted, and keeps it open for appending.
  bool Open(const std::filesystem::path& path, std::vector<StoredShader>& shaders,
            std::vector<StoredPipeline>& pipelines);
  void Close();
  bool is_open() const { return file_ != nullptr; }

  // Appends (and flushes) unless already stored.
  void AddShader(bool is_pixel, uint64_t hash, const uint32_t* raw_dwords, size_t dword_count);
  void AddPipeline(uint64_t vs, uint64_t ps, const PipelineKey& key);

 private:
  void WriteShader(const StoredShader& shader);
  void WritePipeline(const StoredPipeline& pipeline);

  std::FILE* file_ = nullptr;
  std::unordered_set<uint64_t> shaders_;    // hash ^ pixel bit
  std::unordered_set<uint64_t> pipelines_;  // XXH3 of the stored record
};

}  // namespace mantra
