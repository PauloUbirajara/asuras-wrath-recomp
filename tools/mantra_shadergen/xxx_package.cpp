#include "xxx_package.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <lzx.h>
#include <mspack.h>
}

namespace mantra_shadergen {
namespace {

constexpr uint32_t kPackageMagic = 0x9E2A83C1;
constexpr uint32_t kBlockSize = 0x20000;
constexpr int kWindowBits = 17;  // 0x20000 window

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

// In-memory mspack I/O.
struct MemFile {
  const std::vector<uint8_t>* in = nullptr;
  std::vector<uint8_t>* out = nullptr;
  size_t offset = 0;
  size_t capacity = 0;
};
int MemRead(mspack_file* file, void* buffer, int bytes) {
  auto* f = reinterpret_cast<MemFile*>(file);
  const size_t n = std::min<size_t>(size_t(bytes), f->in->size() - f->offset);
  std::memcpy(buffer, f->in->data() + f->offset, n);
  f->offset += n;
  return int(n);
}
int MemWrite(mspack_file* file, void* buffer, int bytes) {
  auto* f = reinterpret_cast<MemFile*>(file);
  const size_t n = std::min<size_t>(size_t(bytes), f->capacity - f->out->size());
  auto* p = static_cast<uint8_t*>(buffer);
  f->out->insert(f->out->end(), p, p + n);
  return int(n);
}
void* MemAlloc(mspack_system*, size_t bytes) { return std::calloc(bytes, 1); }
void MemFree(void* p) { std::free(p); }
void MemCopy(void* src, void* dst, size_t bytes) { std::memcpy(dst, src, bytes); }
void MemMessage(mspack_file*, const char*, ...) {}

mspack_system g_mem_system = {nullptr, nullptr, MemRead,  MemWrite, nullptr, nullptr,
                              MemMessage, MemAlloc, MemFree, MemCopy, nullptr};

// One compressed block: gather the segment bitstreams, then LZX-decode them.
bool DecodeBlock(const uint8_t* p, size_t length, size_t uncompressed, std::vector<uint8_t>& out) {
  std::vector<uint8_t> stream;
  size_t i = 0;
  while (i + 2 <= length) {
    if (p[i] == 0xFF) {  // final segment: FF, u16 uncompressed, u16 compressed
      if (i + 5 > length) {
        return false;
      }
      const uint16_t size = Be16(p + i + 3);
      i += 5;
      if (i + size > length) {
        return false;
      }
      stream.insert(stream.end(), p + i, p + i + size);
      break;
    }
    const uint16_t size = Be16(p + i);
    i += 2;
    if (size == 0) {
      break;
    }
    if (i + size > length) {
      return false;
    }
    stream.insert(stream.end(), p + i, p + i + size);
    i += size;
  }

  std::vector<uint8_t> decoded;
  decoded.reserve(uncompressed);
  MemFile in{&stream, nullptr, 0, 0};
  MemFile dst{nullptr, &decoded, 0, uncompressed};
  lzxd_stream* lzx = lzxd_init(&g_mem_system, reinterpret_cast<mspack_file*>(&in),
                               reinterpret_cast<mspack_file*>(&dst), kWindowBits, 0, 0x8000,
                               off_t(uncompressed), 0);
  if (!lzx) {
    return false;
  }
  const int result = lzxd_decompress(lzx, off_t(uncompressed));
  lzxd_free(lzx);
  if (result != MSPACK_ERR_OK || decoded.size() != uncompressed) {
    return false;
  }
  out.insert(out.end(), decoded.begin(), decoded.end());
  return true;
}

}  // namespace

bool DecompressPackage(const std::vector<uint8_t>& file, std::vector<uint8_t>& out,
                       PackageStats& stats) {
  bool all_ok = true;
  // Compressed chunks: magic, block size 0x20000, summed compressed and
  // uncompressed sizes, then one (compressed, uncompressed) pair per block.
  // Chunks may start at any byte offset.
  for (size_t i = 0; i + 16 <= file.size();) {
    if (Be32(&file[i]) != kPackageMagic || Be32(&file[i + 4]) != kBlockSize) {
      ++i;
      continue;
    }
    const uint32_t total_compressed = Be32(&file[i + 8]);
    const uint32_t total_uncompressed = Be32(&file[i + 12]);
    const uint32_t block_count = (total_uncompressed + kBlockSize - 1) / kBlockSize;
    const size_t table = i + 16;
    const size_t data = table + size_t(block_count) * 8;
    if (!block_count || data + total_compressed > file.size()) {
      ++i;
      continue;
    }
    ++stats.chunks;
    size_t position = data;
    bool chunk_ok = true;
    for (uint32_t b = 0; b < block_count && chunk_ok; ++b) {
      const uint32_t compressed = Be32(&file[table + b * 8]);
      const uint32_t uncompressed = Be32(&file[table + b * 8 + 4]);
      if (position + compressed > file.size() ||
          !DecodeBlock(&file[position], compressed, uncompressed, out)) {
        chunk_ok = false;
      }
      position += compressed;
    }
    if (!chunk_ok) {
      ++stats.failed_chunks;
      all_ok = false;
    }
    i = data + total_compressed;
  }
  return all_ok;
}

}  // namespace mantra_shadergen
