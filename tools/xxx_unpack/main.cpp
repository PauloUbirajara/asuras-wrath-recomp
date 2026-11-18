// xxx_unpack: decompresses Asura's Wrath .xxx packages (UE3 + CC2 LZX framing)
// into plain UE3 package bodies, for asset extraction (tools/asset_extract.py).
//
//   xxx_unpack <out dir> <package.xxx>...
//
// Writes <out dir>/<package>.upk per input. Uses mantra_shadergen's LZX reader.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "xxx_package.h"

namespace fs = std::filesystem;

namespace {

uint32_t Be32(const std::vector<uint8_t>& d, size_t at) {
  return at + 4 <= d.size() ? uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 |
                                  uint32_t(d[at + 2]) << 8 | d[at + 3]
                            : 0;
}

// Packages whose summary is stored uncompressed (version 841 at offset 4):
// returns the uncompressed offset of the first compressed chunk, read from
// the summary's compressed chunk table, or 0 when the whole file is one
// compressed stream (the summary is then inside the decompressed data).
size_t BodyOffset(const std::vector<uint8_t>& f) {
  if (Be32(f, 0) != 0x9E2A83C1 || (Be32(f, 4) & 0xFFFF) != 841) return 0;
  size_t p = 12;
  const int32_t folder_len = int32_t(Be32(f, p));
  p += 4 + (folder_len < 0 ? size_t(-folder_len) * 2 : size_t(folder_len));
  p += 4;          // package flags
  p += 6 * 4;      // name/export/import count+offset
  p += 4;          // depends offset
  p += 3 * 4;      // import/export guids offset + counts
  p += 4;          // thumbnail table offset
  p += 16;         // guid
  const uint32_t generations = Be32(f, p);
  p += 4 + size_t(generations) * 12;
  p += 4 + 4 + 4;  // engine version, cooked content version, compression flags
  const uint32_t chunks = Be32(f, p);
  if (!chunks || chunks > 4096) return 0;
  return Be32(f, p + 4);  // first chunk's uncompressed offset
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <out dir> <package.xxx>...\n", argv[0]);
    return 2;
  }
  const fs::path out_dir = argv[1];
  fs::create_directories(out_dir);
  int failed = 0;
  for (int i = 2; i < argc; ++i) {
    const fs::path in = argv[i];
    std::ifstream f(in, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint8_t> body;
    mantra_shadergen::PackageStats stats;
    const size_t body_offset = BodyOffset(file);
    body.assign(file.begin(), file.begin() + std::min(body_offset, file.size()));
    const bool ok = mantra_shadergen::DecompressPackage(file, body, stats);
    if (!ok) ++failed;
    const fs::path out = out_dir / (in.stem().string() + ".upk");
    std::ofstream(out, std::ios::binary)
        .write(reinterpret_cast<const char*>(body.data()), std::streamsize(body.size()));
    std::printf("%s: %u chunks (%u failed), %zu bytes\n", in.filename().c_str(), stats.chunks,
                stats.failed_chunks, body.size());
  }
  return failed ? 1 : 0;
}
