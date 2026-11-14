// mantra_shadergen: translates Asura's Wrath's Xbox 360 shaders to SPIR-V for
// the mantra renderer (the launcher's "translate shaders" step).
//
//   mantra_shadergen --game <extracted dir> --out <cache file>
//                    [--dumps <dir>] [--hlsl <dir>] [--jobs N]
//                    [--child-mem-mb N] [--child-timeout S] [--scan-only]
//                    [--keep-work] [--only <hash>,<hash>...]
//
// Sources: BCGame/CookedXbox360/GlobalShaderCache-Xbox360.bin and every .xxx
// package (LZX-decompressed), including the LZX-compressed shader code in
// each package's ShaderCache export (the material shaders). Each unique shader goes through XenosRecomp
// (HLSL) and DXC (SPIR-V) in a child process, since XenosRecomp asserts
// instead of reporting unsupported shaders. --dumps reports how many runtime
// dumps (MANTRA_SHADER_DUMP) the cache covers.
//
// Some shaders make XenosRecomp/DXC use tens of GB; each child gets an
// address-space cap (default 6 GB) and a CPU-time limit (default 120 s) so a
// runaway translation fails alone instead of taking the machine down.

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mantra_renderer/shader_cache_format.h"
#include "mantra_renderer/shader_key.h"
#include "translator.h"
#include "xxx_package.h"

namespace fs = std::filesystem;
using namespace mantra_shadergen;

namespace {

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

bool WriteFile(const fs::path& path, const void* data, size_t size) {
  std::ofstream file(path, std::ios::binary);
  file.write(static_cast<const char*>(data), std::streamsize(size));
  return bool(file);
}

struct Shader {
  std::vector<uint8_t> container;
  uint64_t exact_hash = 0;
  uint64_t normalized_hash = 0;
  uint32_t microcode_dwords = 0;
  bool is_pixel = false;
  std::string source;
  // Translation results.
  bool translated = false;
  uint32_t spec_constants_mask = 0;
  uint32_t texture_slot_mask = 0;
  uint32_t bool_mask = 0;
  bool bool_overflow = false;
  uint32_t ps_outputs = 0;
  std::vector<uint32_t> vertex_elements;
  std::vector<uint8_t> spirv;
};

// Written by a translation child next to its .spv.
struct ChildResult {
  uint32_t spec_constants_mask;
  uint32_t texture_slot_mask;
  uint32_t bool_mask;
  uint32_t bool_overflow;
  uint32_t ps_outputs;
  uint32_t vertex_element_count;
  uint32_t vertex_elements[32];
};

void PutBe32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  v[at] = uint8_t(x >> 24); v[at + 1] = uint8_t(x >> 16); v[at + 2] = uint8_t(x >> 8); v[at + 3] = uint8_t(x);
}
void PutBe16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
  v[at] = uint8_t(x >> 8); v[at + 1] = uint8_t(x);
}

// UE3 strips the D3DX constant table from cooked shaders (offset 0x10 is 0),
// but XenosRecomp declares constants, samplers and bools from it. Append a
// synthetic table covering the hardware register files - constants are bound
// by register, so the original parameter names are not needed:
//   C  : float4 c0..c255 (vertex) / c0..c223 (pixel)
//   s0..s31 : samplers (one per fetch constant), b0..b15 : bools
void AddSyntheticConstantTable(std::vector<uint8_t>& container, bool is_pixel) {
  struct Info { std::string name; uint16_t set, index, count; };
  std::vector<Info> infos;
  infos.push_back({"C", 2 /*Float4*/, 0, uint16_t(is_pixel ? 224 : 256)});
  for (uint16_t i = 0; i < 32; ++i) {
    infos.push_back({"s" + std::to_string(i), 3 /*Sampler*/, i, 1});
  }
  for (uint16_t i = 0; i < 16; ++i) {
    infos.push_back({"b" + std::to_string(i), 0 /*Bool*/, i, 1});
  }
  // ConstantTableContainer = u32 size + ConstantTable (7 x u32); offsets in
  // the table are relative to the ConstantTable start.
  constexpr size_t kTableSize = 7 * 4, kInfoSize = 20;
  const size_t base = (container.size() + 3) & ~size_t(3);
  const size_t table = base + 4;
  const size_t info_offset = kTableSize;
  size_t names_offset = info_offset + infos.size() * kInfoSize;
  std::vector<uint8_t> names;
  container.resize(table + names_offset, 0);
  PutBe32(container, table + 12, uint32_t(infos.size()));  // constants
  PutBe32(container, table + 16, uint32_t(info_offset));   // constantInfo
  for (size_t i = 0; i < infos.size(); ++i) {
    const size_t at = table + info_offset + i * kInfoSize;
    PutBe32(container, at, uint32_t(names_offset + names.size()));  // name
    PutBe16(container, at + 4, infos[i].set);
    PutBe16(container, at + 6, infos[i].index);
    PutBe16(container, at + 8, infos[i].count);
    names.insert(names.end(), infos[i].name.begin(), infos[i].name.end());
    names.push_back(0);
  }
  container.insert(container.end(), names.begin(), names.end());
  PutBe32(container, base, uint32_t(container.size() - table));   // container size
  PutBe32(container, table, uint32_t(container.size() - table));  // table size
  PutBe32(container, 0x10, uint32_t(base));                      // constantTableOffset
}

// Finds shader containers at any byte offset (XenosRecomp's container test
// plus bounds checks on the shader header), keyed by microcode hash.
void ScanContainers(const std::vector<uint8_t>& data, const std::string& source,
                    std::map<uint64_t, Shader>& shaders, uint32_t& hits) {
  for (size_t i = 0; i + 36 <= data.size(); ++i) {
    const uint8_t* c = &data[i];
    const uint32_t flags = Be32(c);
    if ((flags & 0xFFFFFF00u) != 0x102A1100u) {
      continue;
    }
    const uint32_t virtual_size = Be32(c + 4), physical_size = Be32(c + 8);
    const uint64_t size = uint64_t(virtual_size) + physical_size;
    if (size > data.size() - i || Be32(c + 0x1C) || Be32(c + 0x20)) {
      continue;
    }
    const uint32_t shader_offset = Be32(c + 0x18);
    if (uint64_t(shader_offset) + 8 > virtual_size) {
      continue;
    }
    const uint32_t microcode_offset = Be32(c + shader_offset);
    const uint32_t microcode_bytes = Be32(c + shader_offset + 4);
    if (!microcode_bytes || microcode_bytes % 4 ||
        uint64_t(microcode_offset) + microcode_bytes > physical_size) {
      continue;
    }
    ++hits;
    const uint8_t* microcode = c + virtual_size + microcode_offset;
    const uint64_t exact = mantra::shader_key::ExactHash(microcode, microcode_bytes);
    auto [it, inserted] = shaders.try_emplace(exact);
    if (inserted) {
      Shader& s = it->second;
      s.container.assign(c, c + size);
      s.exact_hash = exact;
      s.normalized_hash = mantra::shader_key::NormalizedHash(microcode, microcode_bytes);
      s.microcode_dwords = microcode_bytes / 4;
      s.is_pixel = (flags & 1) == 0;
      s.source = source;
      if (!Be32(c + 0x10)) {
        AddSyntheticConstantTable(s.container, s.is_pixel);
      }
    }
    i += size - 1;
  }
}

// Translates every shader, `jobs` child processes at a time. A child writes
// <work>/<index>.spv plus a .ok marker holding the spec constants mask; a
// crash or DXC error just leaves the shader untranslated.
struct ChildLimits {
  uint64_t mem_mb = 6144;
  unsigned timeout_s = 120;
};

void ApplyChildLimits(const ChildLimits& limits) {
  if (limits.mem_mb) {
    const rlim_t bytes = rlim_t(limits.mem_mb) << 20;
    const rlimit as{bytes, bytes};
    setrlimit(RLIMIT_AS, &as);
  }
  if (limits.timeout_s) {
    const rlimit cpu{limits.timeout_s, limits.timeout_s};
    setrlimit(RLIMIT_CPU, &cpu);
    alarm(limits.timeout_s * 2);  // wall-clock backstop
  }
}

void TranslateAll(std::vector<Shader*>& shaders, const fs::path& work, const fs::path& hlsl_dir,
                  unsigned jobs, const ChildLimits& limits) {
  std::unordered_map<pid_t, size_t> running;
  size_t next = 0, done = 0;
  auto collect = [&](pid_t pid, int status) {
    const size_t index = running[pid];
    running.erase(pid);
    Shader& s = *shaders[index];
    const fs::path ok = work / (std::to_string(index) + ".ok");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      std::printf("  %016llX (%s): %s\n", static_cast<unsigned long long>(s.exact_hash),
                  s.source.c_str(),
                  WIFSIGNALED(status)              ? strsignal(WTERMSIG(status))
                  : WEXITSTATUS(status) == 3       ? "vertex inputs not matched"
                  : WEXITSTATUS(status) == 4       ? "DXC failed"
                                                   : "translation failed");
    }
    if (fs::exists(ok)) {
      auto bytes = ReadFile(ok);
      if (bytes.size() == sizeof(ChildResult)) {
        ChildResult cr;
        std::memcpy(&cr, bytes.data(), sizeof(cr));
        s.spec_constants_mask = cr.spec_constants_mask;
        s.texture_slot_mask = cr.texture_slot_mask;
        s.bool_mask = cr.bool_mask;
        s.bool_overflow = cr.bool_overflow != 0;
        s.ps_outputs = cr.ps_outputs;
        s.vertex_elements.assign(cr.vertex_elements,
                                 cr.vertex_elements + std::min<uint32_t>(cr.vertex_element_count, 32));
      }
      s.spirv = ReadFile(work / (std::to_string(index) + ".spv"));
      s.translated = !s.spirv.empty();
    }
    if (++done % 50 == 0 || done == shaders.size()) {
      std::printf("  translated %zu/%zu\n", done, shaders.size());
      std::fflush(stdout);
    }
  };
  while (next < shaders.size() || !running.empty()) {
    while (next < shaders.size() && running.size() < jobs) {
      std::fflush(stdout);
      const pid_t pid = fork();
      if (pid == 0) {
        // Child: keep DXC's error text out of the way.
        ApplyChildLimits(limits);
        std::freopen((work / (std::to_string(next) + ".log")).c_str(), "w", stderr);
        TranslateResult r = TranslateContainer(shaders[next]->container.data());
        if (!hlsl_dir.empty()) {
          char name[64];
          std::snprintf(name, sizeof(name), "%s_%016llX.hlsl", r.is_pixel ? "ps" : "vs",
                        static_cast<unsigned long long>(shaders[next]->exact_hash));
          WriteFile(hlsl_dir / name, r.hlsl.data(), r.hlsl.size());
          // The container too, for comparing with runtime dumps.
          std::snprintf(name, sizeof(name), "%s_%016llX.container", r.is_pixel ? "ps" : "vs",
                        static_cast<unsigned long long>(shaders[next]->exact_hash));
          WriteFile(hlsl_dir / name, shaders[next]->container.data(),
                    shaders[next]->container.size());
        }
        std::fflush(stderr);
        if (!r.ok) {
          _exit(r.failure == TranslateResult::Failure::kStageInterface ? 3
                : r.failure == TranslateResult::Failure::kDxc            ? 4
                                                                         : 2);
        }
        WriteFile(work / (std::to_string(next) + ".spv"), r.spirv.data(), r.spirv.size());
        ChildResult cr{r.spec_constants_mask, r.texture_slot_mask, r.bool_mask,
                       uint32_t(r.bool_overflow), r.ps_outputs,
                       uint32_t(std::min<size_t>(r.vertex_elements.size(), 32)), {}};
        std::copy_n(r.vertex_elements.begin(), cr.vertex_element_count, cr.vertex_elements);
        WriteFile(work / (std::to_string(next) + ".ok"), &cr, sizeof(cr));
        _exit(0);
      }
      if (pid < 0) {
        std::perror("fork");
        return;
      }
      running[pid] = next++;
    }
    int status = 0;
    const pid_t pid = waitpid(-1, &status, 0);
    if (pid > 0 && running.count(pid)) {
      collect(pid, status);
    }
  }
}

bool WriteCache(const fs::path& path, const std::map<uint64_t, Shader>& shaders) {
  using namespace mantra::shader_cache;
  std::vector<CacheEntry> entries;
  std::vector<uint8_t> blobs;
  for (const auto& [hash, s] : shaders) {  // std::map: sorted by exact hash
    CacheEntry e{};
    e.exact_hash = s.exact_hash;
    e.normalized_hash = s.normalized_hash;
    e.flags = (s.is_pixel ? kEntryPixelShader : 0) | (s.translated ? 0 : kEntryFailed);
    e.spec_constants_mask = s.spec_constants_mask;
    e.microcode_dwords = s.microcode_dwords;
    e.texture_slot_mask = s.texture_slot_mask;
    e.bool_mask = s.bool_mask;
    e.ps_outputs = s.ps_outputs;
    if (s.translated) {
      e.spirv_offset = uint32_t(blobs.size());
      e.spirv_size = uint32_t(s.spirv.size());
      blobs.insert(blobs.end(), s.spirv.begin(), s.spirv.end());
      blobs.resize((blobs.size() + 3) & ~size_t(3));
      e.vertex_elements_offset = uint32_t(blobs.size());
      e.vertex_element_count = uint32_t(s.vertex_elements.size());
      const auto* words = reinterpret_cast<const uint8_t*>(s.vertex_elements.data());
      blobs.insert(blobs.end(), words, words + s.vertex_elements.size() * sizeof(uint32_t));
    }
    entries.push_back(e);
  }
  CacheHeader header{};
  std::memcpy(header.magic, kMagic, 4);
  header.version = kVersion;
  header.entry_count = uint32_t(entries.size());
  fs::create_directories(path.parent_path());
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream file(tmp, std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(entries.data()),
               std::streamsize(entries.size() * sizeof(CacheEntry)));
    file.write(reinterpret_cast<const char*>(blobs.data()), std::streamsize(blobs.size()));
    if (!file) {
      return false;
    }
  }
  fs::rename(tmp, path);
  return true;
}

// `known_only`: count shaders found in the game files as if translated (for
// --scan-only).
void ReportDumpCoverage(const fs::path& dumps, const std::map<uint64_t, Shader>& shaders,
                        bool known_only = false) {
  std::unordered_set<uint64_t> exact_ok, normalized_ok, known;
  for (const auto& [hash, s] : shaders) {
    known.insert(s.exact_hash);
    if (s.translated || known_only) {
      exact_ok.insert(s.exact_hash);
      normalized_ok.insert(s.normalized_hash);
    }
  }
  std::map<std::string, std::pair<int, int>> counts;  // kind -> (vs, ps)
  for (const auto& entry : fs::directory_iterator(dumps)) {
    if (entry.path().extension() != ".mc") {
      continue;
    }
    const auto code = ReadFile(entry.path());
    const bool is_pixel = entry.path().filename().string().rfind("ps", 0) == 0;
    const uint64_t exact = mantra::shader_key::ExactHash(code.data(), code.size());
    std::string kind;
    if (exact_ok.count(exact)) {
      kind = "exact";
    } else if (normalized_ok.count(mantra::shader_key::NormalizedHash(code.data(), code.size()))) {
      kind = "normalized";
    } else if (known.count(exact)) {
      kind = "known, translation failed";
    } else {
      kind = "miss";
      std::printf("  miss: %s\n", entry.path().filename().c_str());
    }
    auto& c = counts[kind];
    (is_pixel ? c.second : c.first)++;
  }
  std::printf("runtime dump coverage (vs/ps):\n");
  for (const auto& [kind, c] : counts) {
    std::printf("  %-26s %3d / %3d\n", kind.c_str(), c.first, c.second);
  }
}

}  // namespace

int main(int argc, char** argv) {
  fs::path game, out, dumps, hlsl_dir;
  unsigned jobs = 0;  // 0: pick from cores and available memory
  bool scan_only = false;
  bool keep_work = false;
  std::unordered_set<uint64_t> only;  // --only  // --keep-work: keep the per-shader logs (<work>/<index>.log)
  ChildLimits limits;
  for (int i = 1; i < argc;) {
    const std::string flag = argv[i];
    if (flag == "--scan-only" || flag == "--keep-work") {
      (flag == "--scan-only" ? scan_only : keep_work) = true;
      for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
      --argc;
    } else {
      ++i;
    }
  }
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string arg = argv[i];
    if (arg == "--game") game = argv[i + 1];
    else if (arg == "--out") out = argv[i + 1];
    else if (arg == "--dumps") dumps = argv[i + 1];
    else if (arg == "--hlsl") hlsl_dir = argv[i + 1];
    else if (arg == "--only") {
      // Comma-separated exact hashes: translate just these (debugging).
      std::string list = argv[i + 1];
      for (size_t at = 0; at < list.size();) {
        const size_t comma = std::min(list.find(',', at), list.size());
        only.insert(std::strtoull(list.substr(at, comma - at).c_str(), nullptr, 16));
        at = comma + 1;
      }
    }
    else if (arg == "--jobs") jobs = unsigned(std::max(1, std::atoi(argv[i + 1])));
    else if (arg == "--child-mem-mb") limits.mem_mb = std::strtoull(argv[i + 1], nullptr, 10);
    else if (arg == "--child-timeout") limits.timeout_s = unsigned(std::atoi(argv[i + 1]));
  }
  if (game.empty() || out.empty()) {
    std::fprintf(stderr,
                 "usage: %s --game <extracted dir> --out <cache file> [--dumps <dir>] "
                 "[--hlsl <dir>] [--jobs N] [--child-mem-mb N] [--child-timeout S]\n",
                 argv[0]);
    return 1;
  }
  if (!jobs) {
    jobs = std::max(1u, std::thread::hardware_concurrency());
    const long pages = sysconf(_SC_AVPHYS_PAGES), page_size = sysconf(_SC_PAGESIZE);
    if (limits.mem_mb && pages > 0 && page_size > 0) {
      const uint64_t avail_mb = uint64_t(pages) * uint64_t(page_size) >> 20;
      jobs = std::clamp<unsigned>(unsigned(avail_mb / limits.mem_mb), 1u, jobs);
    }
  }
  const fs::path cooked = game / "BCGame" / "CookedXbox360";
  if (!fs::is_directory(cooked)) {
    std::fprintf(stderr, "not an extracted Asura's Wrath: %s missing\n", cooked.c_str());
    return 1;
  }

  std::map<uint64_t, Shader> shaders;
  uint32_t hits = 0;
  {
    const fs::path cache = cooked / "GlobalShaderCache-Xbox360.bin";
    ScanContainers(ReadFile(cache), cache.filename(), shaders, hits);
    std::printf("GlobalShaderCache: %zu unique shaders\n", shaders.size());
  }
  PackageStats stats;
  uint32_t packages = 0, failed_packages = 0, code_chunks = 0;
  for (const auto& entry : fs::recursive_directory_iterator(game)) {
    if (entry.path().extension() != ".xxx") {
      continue;
    }
    std::vector<uint8_t> body;
    if (!DecompressPackage(ReadFile(entry.path()), body, stats)) {
      ++failed_packages;
    }
    ++packages;
    ScanContainers(body, entry.path().filename(), shaders, hits);
    // Material shaders: UE3 keeps each package's shader code LZX-compressed
    // per shader type inside its ShaderCache export.
    std::vector<uint8_t> code;
    if (const uint32_t chunks = DecompressEmbeddedChunks(body, code)) {
      code_chunks += chunks;
      ScanContainers(code, entry.path().filename(), shaders, hits);
    }
  }
  std::printf("packages: %u read (%u with failed chunks), %u shader code chunks, "
              "%u container hits, %zu unique shaders\n",
              packages, failed_packages, code_chunks, hits, shaders.size());
  if (scan_only) {
    size_t ps = 0;
    for (const auto& [hash, s] : shaders) ps += s.is_pixel;
    std::printf("scan only: %zu vertex + %zu pixel shaders\n", shaders.size() - ps, ps);
    if (!dumps.empty()) {
      ReportDumpCoverage(dumps, shaders, true);
    }
    return 0;
  }

  // Runtime-patched variants: the D3D runtime patches vertex shaders at draw
  // time (vertex fetch formats/offsets, export swizzles and masks for the
  // pixel shader's inputs). Each dumped program whose normalized hash matches
  // a game shader of the same size is translated from that shader's container
  // with the dumped microcode, and cached under its exact hash.
  if (!dumps.empty()) {
    std::unordered_map<uint64_t, const Shader*> by_normalized;
    for (const auto& [hash, s] : shaders) by_normalized.emplace(s.normalized_hash, &s);
    size_t variants = 0;
    std::vector<std::pair<uint64_t, Shader>> added;
    for (const auto& entry : fs::directory_iterator(dumps)) {
      if (entry.path().extension() != ".mc") continue;
      const auto code = ReadFile(entry.path());
      const uint64_t exact = mantra::shader_key::ExactHash(code.data(), code.size());
      if (shaders.count(exact)) continue;
      auto base = by_normalized.find(mantra::shader_key::NormalizedHash(code.data(), code.size()));
      if (base == by_normalized.end()) continue;
      const Shader& original = *base->second;
      const bool is_pixel = entry.path().filename().string().rfind("ps", 0) == 0;
      if (original.is_pixel != is_pixel || original.microcode_dwords * 4 != code.size()) continue;
      Shader variant = original;
      variant.exact_hash = exact;
      variant.source = "patched " + entry.path().filename().string() + " of " + original.source;
      if (!ReplaceMicrocode(variant.container, code)) continue;
      added.emplace_back(exact, std::move(variant));
      ++variants;
    }
    for (auto& [hash, s] : added) shaders.emplace(hash, std::move(s));
    std::printf("runtime-patched variants: %zu\n", variants);
  }

  std::vector<Shader*> list;
  for (auto it = shaders.begin(); it != shaders.end();) {
    if (!only.empty() && !only.count(it->first)) {
      it = shaders.erase(it);
      continue;
    }
    list.push_back(&it->second);
    ++it;
  }
  const fs::path work = fs::temp_directory_path() / ("mantra_shadergen_" + std::to_string(getpid()));
  fs::create_directories(work);
  if (!hlsl_dir.empty()) {
    fs::create_directories(hlsl_dir);
  }
  std::printf("translating with %u jobs...\n", jobs);
  TranslateAll(list, work, hlsl_dir, jobs, limits);
  if (keep_work) {
    std::printf("kept %s\n", work.c_str());
  } else {
    fs::remove_all(work);
  }

  size_t ok_vs = 0, ok_ps = 0, failed = 0;
  for (const auto& [hash, s] : shaders) {
    if (!s.translated) {
      ++failed;
    } else {
      (s.is_pixel ? ok_ps : ok_vs)++;
    }
  }
  std::printf("translated: %zu vertex + %zu pixel shaders, %zu failed\n", ok_vs, ok_ps, failed);
  size_t textured = 0, with_bools = 0, bool_overflow = 0;
  uint32_t slots = 0;
  for (const auto& [hash, s] : shaders) {
    textured += s.texture_slot_mask != 0;
    with_bools += s.bool_mask != 0;
    bool_overflow += s.bool_overflow;
    slots |= s.texture_slot_mask;
  }
  std::printf("interface: %zu shaders sample textures (slots used: %08X), %zu test bools, "
              "%zu test bools outside b0..b15\n",
              textured, slots, with_bools, bool_overflow);

  if (!WriteCache(out, shaders)) {
    std::fprintf(stderr, "failed to write %s\n", out.c_str());
    return 1;
  }
  std::printf("wrote %s\n", out.c_str());
  if (!dumps.empty()) {
    ReportDumpCoverage(dumps, shaders);
  }
  return 0;
}
