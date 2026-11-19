#include "pipeline_store.h"
#include "mantra_log.h"

#include <cstring>
#include <fstream>
#include <iterator>

#include <xxhash.h>

namespace mantra {

namespace {

constexpr char kMagic[4] = {'M', 'P', 'S', 'O'};
constexpr uint32_t kVersion = 1;
constexpr uint8_t kShaderTag = 'S', kPipelineTag = 'P';
constexpr uint32_t kMaxDwords = 0x10000;

uint64_t ShaderId(bool is_pixel, uint64_t hash) {
  return hash ^ (is_pixel ? 0x9E3779B97F4A7C15ull : 0);
}

uint64_t PipelineId(const StoredPipeline& p) {
  struct {
    uint64_t vs, ps;
    PipelineKey key;
  } record;
  std::memset(&record, 0, sizeof(record));
  record.vs = p.vs;
  record.ps = p.ps;
  std::memcpy(&record.key, &p.key, sizeof(p.key));
  return XXH3_64bits(&record, sizeof(record));
}

struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  template <typename T>
  bool Get(T& value) {
    if (size_t(end - p) < sizeof(T)) return false;
    std::memcpy(&value, p, sizeof(T));
    p += sizeof(T);
    return true;
  }
  bool Get(void* data, size_t size) {
    if (size_t(end - p) < size) return false;
    std::memcpy(data, p, size);
    p += size;
    return true;
  }
};

}  // namespace

bool PipelineStore::Open(const std::filesystem::path& path, std::vector<StoredShader>& shaders,
                         std::vector<StoredPipeline>& pipelines) {
  Close();
  shaders.clear();
  pipelines.clear();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  std::vector<uint8_t> bytes;
  if (std::ifstream in(path, std::ios::binary); in) {
    bytes.assign(std::istreambuf_iterator<char>(in), {});
  }
  Reader r{bytes.data(), bytes.data() + bytes.size()};
  char magic[4];
  uint32_t version = 0, key_size = 0;
  if (r.Get(magic, 4) && !std::memcmp(magic, kMagic, 4) && r.Get(version) &&
      version == kVersion && r.Get(key_size) && key_size == sizeof(PipelineKey)) {
    // Records until the end or a truncated/garbled one (a killed process).
    for (uint8_t tag; r.Get(tag);) {
      if (tag == kShaderTag) {
        StoredShader s;
        uint8_t is_pixel;
        uint32_t count;
        if (!r.Get(is_pixel) || !r.Get(s.hash) || !r.Get(count) || !count || count > kMaxDwords) {
          break;
        }
        s.is_pixel = is_pixel != 0;
        s.ucode.resize(count);
        if (!r.Get(s.ucode.data(), count * sizeof(uint32_t))) break;
        if (XXH3_64bits(s.ucode.data(), count * sizeof(uint32_t)) != s.hash) break;
        if (shaders_.insert(ShaderId(s.is_pixel, s.hash)).second) {
          shaders.push_back(std::move(s));
        }
      } else if (tag == kPipelineTag) {
        StoredPipeline p;
        std::memset(&p, 0, sizeof(p));
        if (!r.Get(p.vs) || !r.Get(p.ps) || !r.Get(&p.key, sizeof(p.key))) break;
        if (pipelines_.insert(PipelineId(p)).second) {
          pipelines.push_back(p);
        }
      } else {
        break;
      }
    }
  } else if (!bytes.empty()) {
    MANTRA_INFO("pipeline store {}: other format version, starting over", path.string());
  }

  // Rewrite compacted (dropping a truncated tail), then append from there.
  const std::filesystem::path temp = path.string() + ".tmp";
  std::FILE* file = std::fopen(temp.string().c_str(), "wb");
  if (!file) {
    MANTRA_WARN("pipeline store: cannot write {}", temp.string());
    shaders_.clear();
    pipelines_.clear();
    return false;
  }
  file_ = file;
  std::fwrite(kMagic, 1, 4, file_);
  std::fwrite(&kVersion, sizeof(kVersion), 1, file_);
  const uint32_t size = sizeof(PipelineKey);
  std::fwrite(&size, sizeof(size), 1, file_);
  for (const StoredShader& s : shaders) WriteShader(s);
  for (const StoredPipeline& p : pipelines) WritePipeline(p);
  std::fflush(file_);
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    MANTRA_WARN("pipeline store: cannot replace {}: {}", path.string(), ec.message());
    Close();
    return false;
  }
  return true;
}

void PipelineStore::Close() {
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  shaders_.clear();
  pipelines_.clear();
}

void PipelineStore::WriteShader(const StoredShader& s) {
  const uint8_t tag = kShaderTag, is_pixel = s.is_pixel;
  const uint32_t count = uint32_t(s.ucode.size());
  std::fwrite(&tag, 1, 1, file_);
  std::fwrite(&is_pixel, 1, 1, file_);
  std::fwrite(&s.hash, sizeof(s.hash), 1, file_);
  std::fwrite(&count, sizeof(count), 1, file_);
  std::fwrite(s.ucode.data(), sizeof(uint32_t), count, file_);
}

void PipelineStore::WritePipeline(const StoredPipeline& p) {
  const uint8_t tag = kPipelineTag;
  std::fwrite(&tag, 1, 1, file_);
  std::fwrite(&p.vs, sizeof(p.vs), 1, file_);
  std::fwrite(&p.ps, sizeof(p.ps), 1, file_);
  std::fwrite(&p.key, sizeof(p.key), 1, file_);
}

void PipelineStore::AddShader(bool is_pixel, uint64_t hash, const uint32_t* raw_dwords,
                              size_t dword_count) {
  if (!file_ || !dword_count || dword_count > kMaxDwords ||
      !shaders_.insert(ShaderId(is_pixel, hash)).second) {
    return;
  }
  StoredShader s;
  s.is_pixel = is_pixel;
  s.hash = hash;
  s.ucode.assign(raw_dwords, raw_dwords + dword_count);
  WriteShader(s);
  std::fflush(file_);
}

void PipelineStore::AddPipeline(uint64_t vs, uint64_t ps, const PipelineKey& key) {
  if (!file_) {
    return;
  }
  StoredPipeline p;
  std::memset(&p, 0, sizeof(p));
  p.vs = vs;
  p.ps = ps;
  std::memcpy(&p.key, &key, sizeof(key));
  p.key.vertex_module = 0;
  p.key.pixel_module = 0;
  if (!pipelines_.insert(PipelineId(p)).second) {
    return;
  }
  WritePipeline(p);
  std::fflush(file_);
}

}  // namespace mantra
