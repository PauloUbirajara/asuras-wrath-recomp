// mantra_shadergen: translates Asura's Wrath's Xbox 360 shaders to SPIR-V for
// the mantra renderer (the launcher's "translate shaders" step).
//
//   mantra_shadergen --game <extracted dir> --out <cache file>
//                    [--dumps <dir>]... [--hlsl <dir>] [--jobs N]
//                    [--child-mem-mb N] [--child-timeout S] [--scan-only]
//                    [--keep-work] [--only <hash>,<hash>...]
//
// Sources: BCGame/CookedXbox360/GlobalShaderCache-Xbox360.bin and every .xxx
// package (LZX-decompressed), including the LZX-compressed shader code in
// each package's ShaderCache export (the material shaders). Each unique shader goes through XenosRecomp
// (HLSL) and DXC (SPIR-V) in a child process, since XenosRecomp asserts
// instead of reporting unsupported shaders. A child is this tool run again
// with --worker (no fork, so it works on Windows too). --dumps reports how many runtime
// dumps (MANTRA_SHADER_DUMP) the cache covers.
//
// Some shaders make XenosRecomp/DXC use tens of GB; each child gets an
// address-space cap (default 6 GB) and a CPU-time limit (default 120 s) so a
// runaway translation fails alone instead of taking the machine down.

#ifndef _WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mantra_renderer/shader_cache_format.h"
#include "mantra_renderer/shader_key.h"
#include "platform/platform.h"
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
  bool from_cache = false;  // --update: results taken from the existing cache
  bool attempted = false;   // translated (or failed) in this run
  uint32_t spec_constants_mask = 0;
  uint32_t texture_slot_mask = 0;
  uint32_t bool_mask = 0;
  bool bool_overflow = false;
  uint32_t ps_outputs = 0;
  uint32_t vertex_index_divisor = 0;
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
  uint32_t vertex_index_divisor;
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

// Stopping: the launcher's Stop creates <out>.stop; --parent <pid> stops
// too once that process (the launcher) is gone, e.g. closed or crashed.
fs::path g_stop_file;
int g_parent_pid = 0;
bool StopRequested() {
  static bool stop = false;
  static auto checked = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (stop || now - checked < std::chrono::milliseconds(250)) return stop;
  checked = now;
  std::error_code ec;
  stop = (!g_stop_file.empty() && fs::exists(g_stop_file, ec)) ||
         (g_parent_pid && !asura::platform::ProcessAlive(g_parent_pid));
  return stop;
}
constexpr int kExitStopped = 5;

// Translates every shader, `jobs` child processes at a time. A child writes
// <work>/<index>.spv plus a .ok marker holding the spec constants mask; a
// crash or DXC error just leaves the shader untranslated.
struct ChildLimits {
  uint64_t mem_mb = 6144;
  unsigned timeout_s = 120;
};

// In the worker. Windows has no address-space or CPU-time limit per process
// here: the parent's wall-clock timeout (TranslateAll) is the only backstop.
void ApplyChildLimits(const ChildLimits& limits) {
#ifndef _WIN32
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
#else
  (void)limits;
#endif
}

// --worker <work> <index> <hash> [<hlsl dir>]: translates <work>/<index>.container
// into <work>/<index>.spv and the .ok marker. Exit 0 = translated; 2, 3, 4 =
// translation failed, vertex inputs not matched, DXC failed.
int RunWorker(const fs::path& work, const std::string& index, uint64_t exact_hash,
              const fs::path& hlsl_dir, const ChildLimits& limits) {
  ApplyChildLimits(limits);
  // Keep DXC's error text out of the way.
  std::freopen((work / (index + ".log")).string().c_str(), "w", stderr);
  const std::vector<uint8_t> container = ReadFile(work / (index + ".container"));
  if (container.empty()) return 2;
  TranslateResult r = TranslateContainer(container.data());
  if (!hlsl_dir.empty()) {
    char name[64];
    std::snprintf(name, sizeof(name), "%s_%016llX.hlsl", r.is_pixel ? "ps" : "vs",
                  static_cast<unsigned long long>(exact_hash));
    WriteFile(hlsl_dir / name, r.hlsl.data(), r.hlsl.size());
    // The container too, for comparing with runtime dumps.
    std::snprintf(name, sizeof(name), "%s_%016llX.container", r.is_pixel ? "ps" : "vs",
                  static_cast<unsigned long long>(exact_hash));
    WriteFile(hlsl_dir / name, container.data(), container.size());
  }
  std::fflush(stderr);
  if (!r.ok) {
    return r.failure == TranslateResult::Failure::kStageInterface ? 3
           : r.failure == TranslateResult::Failure::kDxc          ? 4
                                                                  : 2;
  }
  WriteFile(work / (index + ".spv"), r.spirv.data(), r.spirv.size());
  ChildResult cr{r.spec_constants_mask, r.texture_slot_mask, r.bool_mask,
                 uint32_t(r.bool_overflow), r.ps_outputs, r.vertex_index_divisor,
                 uint32_t(std::min<size_t>(r.vertex_elements.size(), 32)), {}};
  std::copy_n(r.vertex_elements.begin(), cr.vertex_element_count, cr.vertex_elements);
  WriteFile(work / (index + ".ok"), &cr, sizeof(cr));
  return 0;
}

// `checkpoint` runs every 30 s or more and when stopped (saving the progress);
// `base` shaders were done before (a resumed build), for the progress lines.
// Returns false when stopped.
bool TranslateAll(std::vector<Shader*>& shaders, const fs::path& work, const fs::path& hlsl_dir,
                  unsigned jobs, const ChildLimits& limits, size_t base,
                  const std::function<void()>& checkpoint) {
  using Clock = std::chrono::steady_clock;
#ifdef __ANDROID__
  // platform::ExePath() is the game's stand-in on Android; this tool is a
  // real program there (libmantra_shadergen.so in the app's library folder).
  std::error_code exe_ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", exe_ec);
#else
  const fs::path exe = asura::platform::ExePath();
#endif
  struct Running {
    std::unique_ptr<asura::platform::Process> process;
    size_t index;
    Clock::time_point started;
  };
  std::vector<Running> running;
  size_t next = 0, done = 0;
  auto collect = [&](size_t index, const asura::platform::ExitStatus& status, bool timed_out) {
    Shader& s = *shaders[index];
    s.attempted = true;
    const fs::path ok = work / (std::to_string(index) + ".ok");
    if (!status.Ok()) {
#ifndef _WIN32
      const char* killed = !status.normal ? strsignal(status.code) : nullptr;
#else
      const char* killed = nullptr;
#endif
      std::printf("  %016llX (%s): %s\n", static_cast<unsigned long long>(s.exact_hash),
                  s.source.c_str(),
                  timed_out              ? "timed out"
                  : killed               ? killed
                  : !status.normal       ? "killed"
                  : status.code == 3     ? "vertex inputs not matched"
                  : status.code == 4     ? "DXC failed"
                                         : "translation failed");
    }
    if (status.Ok() && fs::exists(ok)) {
      auto bytes = ReadFile(ok);
      if (bytes.size() == sizeof(ChildResult)) {
        ChildResult cr;
        std::memcpy(&cr, bytes.data(), sizeof(cr));
        s.spec_constants_mask = cr.spec_constants_mask;
        s.texture_slot_mask = cr.texture_slot_mask;
        s.bool_mask = cr.bool_mask;
        s.bool_overflow = cr.bool_overflow != 0;
        s.ps_outputs = cr.ps_outputs;
        s.vertex_index_divisor = cr.vertex_index_divisor;
        s.vertex_elements.assign(cr.vertex_elements,
                                 cr.vertex_elements + std::min<uint32_t>(cr.vertex_element_count, 32));
      }
      s.spirv = ReadFile(work / (std::to_string(index) + ".spv"));
      s.translated = !s.spirv.empty();
    }
    std::error_code ec;
    fs::remove(work / (std::to_string(index) + ".container"), ec);
    if (++done % 50 == 0 || done == shaders.size()) {
      std::printf("  translated %zu/%zu\n", base + done, base + shaders.size());
      std::fflush(stdout);
    }
  };
  // Wall clock per worker: twice the CPU limit (as the POSIX alarm).
  const auto timeout = std::chrono::seconds(limits.timeout_s ? limits.timeout_s * 2 : 0);
  auto saved_at = Clock::now();
  Clock::duration save_every = std::chrono::seconds(30);
  while (next < shaders.size() || !running.empty()) {
    if (StopRequested()) {
      // The running workers' shaders stay untried: translated next time.
      for (Running& r : running) {
        r.process->Kill();
        r.process->Wait();
      }
      checkpoint();
      return false;
    }
    // The file grows to a few hundred MB: save at most 1/20 of the time.
    if (Clock::now() - saved_at > save_every) {
      const auto start = Clock::now();
      checkpoint();
      saved_at = Clock::now();
      save_every = std::max<Clock::duration>(std::chrono::seconds(30), (saved_at - start) * 20);
    }
    while (next < shaders.size() && running.size() < jobs) {
      const std::string index = std::to_string(next);
      const Shader& shader = *shaders[next];
      WriteFile(work / (index + ".container"), shader.container.data(), shader.container.size());
      char hash[24];
      std::snprintf(hash, sizeof(hash), "%016llX", static_cast<unsigned long long>(shader.exact_hash));
      std::vector<std::string> args = {exe.string(), "--worker", work.string(), index, hash,
                                       "--child-mem-mb", std::to_string(limits.mem_mb),
                                       "--child-timeout", std::to_string(limits.timeout_s)};
      if (!hlsl_dir.empty()) {
        args.push_back("--hlsl");
        args.push_back(hlsl_dir.string());
      }
      auto process = std::make_unique<asura::platform::Process>();
      std::string error;
      if (!process->Start(exe, args, /*capture_output=*/false, &error)) {
        std::fprintf(stderr, "cannot start a worker: %s\n", error.c_str());
        return true;
      }
      running.push_back({std::move(process), next++, Clock::now()});
    }
    bool any = false;
    for (size_t i = 0; i < running.size();) {
      Running& r = running[i];
      std::optional<asura::platform::ExitStatus> status = r.process->TryWait();
      bool timed_out = false;
      if (!status && timeout.count() && Clock::now() - r.started > timeout) {
        r.process->Kill();
        status = r.process->Wait();
        timed_out = true;
      }
      if (!status) {
        ++i;
        continue;
      }
      collect(r.index, *status, timed_out);
      running.erase(running.begin() + std::ptrdiff_t(i));
      any = true;
    }
    if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

// `tried_only`: just the shaders with a result (a stopped build's progress;
// the others would read as failed).
bool WriteCache(const fs::path& path, const std::map<uint64_t, Shader>& shaders,
                bool tried_only = false) {
  using namespace mantra::shader_cache;
  std::vector<CacheEntry> entries;
  std::vector<uint8_t> blobs;
  for (const auto& [hash, s] : shaders) {  // std::map: sorted by exact hash
    if (tried_only && !s.attempted && !s.from_cache) continue;
    CacheEntry e{};
    e.exact_hash = s.exact_hash;
    e.normalized_hash = s.normalized_hash;
    e.flags = (s.is_pixel ? kEntryPixelShader : 0) | (s.translated ? 0 : kEntryFailed);
    e.spec_constants_mask = s.spec_constants_mask;
    e.microcode_dwords = s.microcode_dwords;
    e.texture_slot_mask = s.texture_slot_mask;
    e.bool_mask = s.bool_mask;
    e.ps_outputs = s.ps_outputs;
    e.vertex_index_divisor = s.vertex_index_divisor;
    if (s.translated) {
      e.spirv_offset = uint32_t(blobs.size());
      e.spirv_size = uint32_t(s.spirv.size());
      blobs.insert(blobs.end(), s.spirv.begin(), s.spirv.end());
      blobs.resize((blobs.size() + 3) & ~size_t(3));
      e.vertex_elements_offset = uint32_t(blobs.size());
      e.vertex_element_count = uint32_t(s.vertex_elements.size());
      const auto* words = reinterpret_cast<const uint8_t*>(s.vertex_elements.data());
      blobs.insert(blobs.end(), words, words + s.vertex_elements.size() * sizeof(uint32_t));
      if (!s.is_pixel) {
        // The exports of the microcode this was translated from, for
        // renumbering the outputs of normalized matches at runtime.
        const std::vector<uint8_t> microcode = ContainerMicrocode(s.container);
        std::vector<uint32_t> export_signatures;
        const std::vector<uint32_t> exports = mantra::shader_key::InterpolatorExports(
            microcode.data(), microcode.size(), &export_signatures);
        e.export_count = uint32_t(exports.size());
        const auto* export_words = reinterpret_cast<const uint8_t*>(exports.data());
        blobs.insert(blobs.end(), export_words, export_words + exports.size() * sizeof(uint32_t));
        // v7: the destination swizzle of each element's container fetch.
        std::vector<uint32_t> swizzles;
        for (uint32_t packed : s.vertex_elements) {
          const size_t at = (size_t(VertexElement::Unpack(packed).address) * 3 + 1) * 4;
          uint32_t swizzle = 0x688;  // xyzw
          if (at + 4 <= microcode.size()) {
            const uint8_t* w = microcode.data() + at;
            const uint32_t word1 = uint32_t(w[0]) << 24 | uint32_t(w[1]) << 16 |
                                   uint32_t(w[2]) << 8 | w[3];
            swizzle = word1 & 0xFFF;  // bits 0..11
            // v10: bits 16..21 = data format; 10:11:11 (16) or 11:11:10 (17)
            // means XenosRecomp decodes this input in the shader.
            const uint32_t format = (word1 >> 16) & 0x3F;
            if (format == 16 || format == 17) swizzle |= mantra::shader_cache::kSwizzlePackedFormat;
          }
          swizzles.push_back(swizzle);
        }
        const auto* swizzle_words = reinterpret_cast<const uint8_t*>(swizzles.data());
        blobs.insert(blobs.end(), swizzle_words, swizzle_words + swizzles.size() * sizeof(uint32_t));
        // v9: what each export computes, to pick among normalized matches.
        const auto* signature_words = reinterpret_cast<const uint8_t*>(export_signatures.data());
        blobs.insert(blobs.end(), signature_words,
                     signature_words + export_signatures.size() * sizeof(uint32_t));
      }
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

// --update: takes the results of every shader already in `path` (same cache
// version only), so only the new ones are translated. Returns how many were
// taken, or -1 when the file can't be used (missing or another version).
int ReadExistingCache(const fs::path& path, std::map<uint64_t, Shader>& shaders) {
  using namespace mantra::shader_cache;
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return -1;
  const std::vector<uint8_t> data = ReadFile(path);
  if (data.size() < sizeof(CacheHeader)) return -1;
  CacheHeader header;
  std::memcpy(&header, data.data(), sizeof(header));
  if (std::memcmp(header.magic, kMagic, 4) != 0 || header.version != kVersion) return -1;
  const size_t entries_at = sizeof(CacheHeader);
  const size_t blobs_at = entries_at + size_t(header.entry_count) * sizeof(CacheEntry);
  if (blobs_at > data.size()) return -1;
  const size_t blob_size = data.size() - blobs_at;
  int taken = 0;
  for (uint32_t i = 0; i < header.entry_count; ++i) {
    CacheEntry e;
    std::memcpy(&e, data.data() + entries_at + size_t(i) * sizeof(CacheEntry), sizeof(e));
    auto it = shaders.find(e.exact_hash);
    if (it == shaders.end()) continue;  // no longer in the game files or the dumps
    Shader& sh = it->second;
    if (sh.is_pixel != ((e.flags & kEntryPixelShader) != 0)) continue;
    const bool failed = (e.flags & kEntryFailed) != 0;
    if (!failed) {
      const size_t elements_size = size_t(e.vertex_element_count) * sizeof(uint32_t);
      if (size_t(e.spirv_offset) + e.spirv_size > blob_size ||
          size_t(e.vertex_elements_offset) + elements_size > blob_size) {
        continue;
      }
      const uint8_t* blobs = data.data() + blobs_at;
      sh.spirv.assign(blobs + e.spirv_offset, blobs + e.spirv_offset + e.spirv_size);
      sh.vertex_elements.resize(e.vertex_element_count);
      if (elements_size) std::memcpy(sh.vertex_elements.data(), blobs + e.vertex_elements_offset, elements_size);
    }
    // Failed ones stay failed: the same translator would fail again.
    sh.translated = !failed;
    sh.from_cache = true;
    sh.spec_constants_mask = e.spec_constants_mask;
    sh.texture_slot_mask = e.texture_slot_mask;
    sh.bool_mask = e.bool_mask;
    sh.ps_outputs = e.ps_outputs;
    sh.vertex_index_divisor = e.vertex_index_divisor;
    ++taken;
  }
  return taken;
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
      std::printf("  miss: %s\n", entry.path().filename().string().c_str());
    }
    auto& c = counts[kind];
    (is_pixel ? c.second : c.first)++;
  }
  std::printf("runtime dump coverage (vs/ps):\n");
  for (const auto& [kind, c] : counts) {
    std::printf("  %-26s %3d / %3d\n", kind.c_str(), c.first, c.second);
  }
}


// --- Scan index (shader_scan.bin) ----------------------------------------------
// Every shader container found in the game files, so a build doesn't have to
// read the 3 GB of packages again. Valid while the files' names, sizes and
// times are the same.
struct ScanCounts {
  uint32_t hits = 0, packages = 0, failed_packages = 0, code_chunks = 0;
};
constexpr char kScanMagic[4] = {'M', 'S', 'C', 'N'};
constexpr uint32_t kScanVersion = 1;

uint64_t ScanSignature(const std::vector<fs::path>& files) {
  std::vector<uint8_t> bytes;
  auto put = [&](const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    bytes.insert(bytes.end(), p, p + size);
  };
  for (const fs::path& file : files) {
    std::error_code ec;
    const std::string name = file.string();
    const uint64_t size = fs::file_size(file, ec);
    const int64_t time = fs::last_write_time(file, ec).time_since_epoch().count();
    put(name.data(), name.size());
    put(&size, sizeof(size));
    put(&time, sizeof(time));
  }
  put(&kScanVersion, sizeof(kScanVersion));
  return mantra::shader_key::ExactHash(bytes.data(), bytes.size());
}

bool SaveScan(const fs::path& path, uint64_t signature, const std::map<uint64_t, Shader>& shaders,
              const ScanCounts& counts) {
  std::vector<uint8_t> out;
  auto put = [&](const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + size);
  };
  auto put32 = [&](uint32_t v) { put(&v, 4); };
  put(kScanMagic, 4);
  put32(kScanVersion);
  put(&signature, 8);
  put(&counts, sizeof(counts));
  put32(uint32_t(shaders.size()));
  for (const auto& [hash, s] : shaders) {
    put(&s.exact_hash, 8);
    put(&s.normalized_hash, 8);
    put32(s.microcode_dwords);
    put32(s.is_pixel);
    put32(uint32_t(s.source.size()));
    put(s.source.data(), s.source.size());
    put32(uint32_t(s.container.size()));
    put(s.container.data(), s.container.size());
  }
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path tmp = path.string() + ".tmp";
  if (!WriteFile(tmp, out.data(), out.size())) return false;
  fs::rename(tmp, path, ec);
  return !ec;
}

bool LoadScan(const fs::path& path, uint64_t signature, std::map<uint64_t, Shader>& shaders,
              ScanCounts& counts) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return false;
  const std::vector<uint8_t> data = ReadFile(path);
  size_t at = 0;
  auto get = [&](void* dest, size_t size) {
    if (size > data.size() - at) return false;
    std::memcpy(dest, data.data() + at, size);
    at += size;
    return true;
  };
  char magic[4];
  uint32_t version = 0, count = 0;
  uint64_t stored = 0;
  if (!get(magic, 4) || std::memcmp(magic, kScanMagic, 4) || !get(&version, 4) ||
      version != kScanVersion || !get(&stored, 8) || stored != signature ||
      !get(&counts, sizeof(counts)) || !get(&count, 4)) {
    return false;
  }
  std::map<uint64_t, Shader> loaded;
  for (uint32_t i = 0; i < count; ++i) {
    Shader s;
    uint32_t is_pixel = 0, size = 0;
    if (!get(&s.exact_hash, 8) || !get(&s.normalized_hash, 8) || !get(&s.microcode_dwords, 4) ||
        !get(&is_pixel, 4) || !get(&size, 4) || size > data.size() - at) {
      return false;
    }
    s.is_pixel = is_pixel != 0;
    s.source.assign(reinterpret_cast<const char*>(data.data() + at), size);
    at += size;
    if (!get(&size, 4) || size > data.size() - at) return false;
    s.container.assign(data.begin() + at, data.begin() + at + size);
    at += size;
    loaded.emplace(s.exact_hash, std::move(s));
  }
  shaders = std::move(loaded);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 5 && std::string_view(argv[1]) == "--worker") {
    ChildLimits limits;
    fs::path hlsl_dir;
    for (int i = 5; i + 1 < argc; i += 2) {
      const std::string_view arg = argv[i];
      if (arg == "--child-mem-mb") limits.mem_mb = std::strtoull(argv[i + 1], nullptr, 10);
      else if (arg == "--child-timeout") limits.timeout_s = unsigned(std::atoi(argv[i + 1]));
      else if (arg == "--hlsl") hlsl_dir = argv[i + 1];
    }
    return RunWorker(argv[2], argv[3], std::strtoull(argv[4], nullptr, 16), hlsl_dir, limits);
  }
  fs::path game, out, hlsl_dir;
  std::vector<fs::path> dumps;  // --dumps, repeatable
  unsigned jobs = 0;  // 0: pick from cores and available memory
  bool scan_only = false;
  bool keep_work = false;
  bool update = false;  // --update: keep what the --out cache already has
  // --requested-only: translate only the shaders the game used (the --dumps
  // programs); the others are left out of the cache, for a later full build.
  bool requested_only = false;
  // --background: while the game runs: lower priority, half the cores.
  bool background = false;
  // --fresh: start over, dropping a stopped build's progress (<out>.partial).
  bool fresh = false;
  fs::path scan_cache;  // --scan-cache (default <out dir>/shader_scan.bin)
  std::unordered_set<uint64_t> only;  // --only  // --keep-work: keep the per-shader logs (<work>/<index>.log)
  ChildLimits limits;
  for (int i = 1; i < argc;) {
    const std::string flag = argv[i];
    bool* set = flag == "--scan-only"        ? &scan_only
                : flag == "--keep-work"      ? &keep_work
                : flag == "--update"         ? &update
                : flag == "--requested-only" ? &requested_only
                : flag == "--background"     ? &background
                : flag == "--fresh"          ? &fresh
                                             : nullptr;
    if (set) {
      *set = true;
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
    else if (arg == "--dumps") {
      // Missing folders are fine (e.g. no game run yet).
      std::error_code ec;
      if (fs::is_directory(argv[i + 1], ec)) dumps.emplace_back(argv[i + 1]);
    }
    else if (arg == "--hlsl") hlsl_dir = argv[i + 1];
    else if (arg == "--scan-cache") scan_cache = argv[i + 1];
    else if (arg == "--parent") g_parent_pid = std::atoi(argv[i + 1]);
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
                 "usage: %s --game <extracted dir> --out <cache file> [--dumps <dir>]... "
                 "[--update] [--requested-only] [--background] [--fresh] [--parent <pid>] [--scan-cache <file>] [--hlsl <dir>] [--jobs N] [--child-mem-mb N] [--child-timeout S]\n",
                 argv[0]);
    return 1;
  }
  if (background) {
    asura::platform::LowerProcessPriority();  // the workers inherit it
  }
  g_stop_file = out.string() + ".stop";
  {
    std::error_code ec;
    fs::remove(g_stop_file, ec);
  }
  if (!jobs) {
    jobs = std::max(1u, std::thread::hardware_concurrency() / (background ? 2 : 1));
    // MemAvailable (free memory plus reclaimable cache; _SC_AVPHYS_PAGES only
    // counts free pages). The child limit is an address-space cap, far above
    // what a translation actually uses, so budget ~1 GB per child.
    const unsigned long long avail_kb = asura::platform::AvailableMemoryBytes() >> 10;
    if (avail_kb) {
      jobs = std::clamp<unsigned>(unsigned((avail_kb >> 10) / 1024), 1u, jobs);
    }
  }
  const fs::path cooked = game / "BCGame" / "CookedXbox360";
  if (!fs::is_directory(cooked)) {
    std::fprintf(stderr, "not an extracted Asura's Wrath: %s missing\n", cooked.string().c_str());
    return 1;
  }

  std::map<uint64_t, Shader> shaders;
  uint32_t hits = 0;
  uint32_t packages = 0, failed_packages = 0, code_chunks = 0;
  std::vector<fs::path> package_paths;
  for (const auto& entry : fs::recursive_directory_iterator(game)) {
    if (entry.path().extension() == ".xxx") package_paths.push_back(entry.path());
  }
  const fs::path global_cache = cooked / "GlobalShaderCache-Xbox360.bin";
  // What was found is kept next to the cache (shader_scan.bin), so later
  // builds (the launcher's, while the game runs) skip reading the packages.
  if (scan_cache.empty()) scan_cache = out.parent_path() / "shader_scan.bin";
  std::vector<fs::path> scanned_files = package_paths;
  scanned_files.push_back(global_cache);
  const uint64_t scan_signature = ScanSignature(scanned_files);
  ScanCounts counts;
  if (LoadScan(scan_cache, scan_signature, shaders, counts)) {
    hits = counts.hits;
    packages = counts.packages;
    failed_packages = counts.failed_packages;
    code_chunks = counts.code_chunks;
    std::printf("read %s\n", scan_cache.string().c_str());
  } else {
  ScanContainers(ReadFile(global_cache), global_cache.filename().string(), shaders, hits);
  std::printf("GlobalShaderCache: %zu unique shaders\n", shaders.size());
  // The packages are independent: read them on `jobs` threads, each into its
  // own map, and merge the maps in directory order so the first package that
  // has a shader still provides it (the same result as one thread).
  struct PackageScan {
    std::map<uint64_t, Shader> shaders;
    uint32_t hits = 0, code_chunks = 0, chunks = 0, failed_chunks = 0;
    bool failed = false, ready = false;
  };
  std::vector<PackageScan> scans(package_paths.size());
  PackageStats stats;
  {
    std::mutex mutex;
    size_t next = 0, merged = 0;
    auto merge_ready = [&] {  // under `mutex`
      while (merged < scans.size() && scans[merged].ready) {
        PackageScan& scan = scans[merged++];
        for (auto& [hash, shader] : scan.shaders) shaders.try_emplace(hash, std::move(shader));
        hits += scan.hits;
        code_chunks += scan.code_chunks;
        stats.chunks += scan.chunks;
        stats.failed_chunks += scan.failed_chunks;
        failed_packages += scan.failed;
        ++packages;
        scan = PackageScan{};
        scan.ready = true;
        if (packages % 20 == 0 || packages == scans.size()) {
          std::printf("  read %u/%zu packages\n", packages, scans.size());
          std::fflush(stdout);
        }
      }
    };
    auto work = [&] {
      for (;;) {
        if (StopRequested()) {
          std::printf("stopped while reading the game files\n");
          std::fflush(stdout);
          std::_Exit(kExitStopped);
        }
        size_t i;
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (next == package_paths.size()) return;
          i = next++;
        }
        PackageScan scan;
        const std::string name = package_paths[i].filename().string();
        std::vector<uint8_t> body;
        PackageStats package_stats;
        scan.failed = !DecompressPackage(ReadFile(package_paths[i]), body, package_stats);
        scan.chunks = package_stats.chunks;
        scan.failed_chunks = package_stats.failed_chunks;
        ScanContainers(body, name, scan.shaders, scan.hits);
        // Material shaders: UE3 keeps each package's shader code LZX-compressed
        // per shader type inside its ShaderCache export.
        std::vector<uint8_t> code;
        if ((scan.code_chunks = DecompressEmbeddedChunks(body, code))) {
          ScanContainers(code, name, scan.shaders, scan.hits);
        }
        scan.ready = true;
        std::lock_guard<std::mutex> lock(mutex);
        scans[i] = std::move(scan);
        merge_ready();
      }
    };
    std::vector<std::thread> threads;
    const unsigned count = std::max(1u, std::min<unsigned>(jobs, unsigned(package_paths.size())));
    for (unsigned t = 1; t < count; ++t) threads.emplace_back(work);
    work();
    for (auto& t : threads) t.join();
  }
  counts = {hits, packages, failed_packages, code_chunks};
  if (!SaveScan(scan_cache, scan_signature, shaders, counts)) {
    std::fprintf(stderr, "could not write %s\n", scan_cache.string().c_str());
  }
  }
  std::printf("packages: %u read (%u with failed chunks), %u shader code chunks, "
              "%u container hits, %zu unique shaders\n",
              packages, failed_packages, code_chunks, hits, shaders.size());
  if (scan_only) {
    size_t ps = 0;
    for (const auto& [hash, s] : shaders) ps += s.is_pixel;
    std::printf("scan only: %zu vertex + %zu pixel shaders\n", shaders.size() - ps, ps);
    // Identifies the scan result (which container and package each shader
    // came from), to compare scans.
    std::vector<uint8_t> all;
    for (const auto& [hash, s] : shaders) {
      all.insert(all.end(), reinterpret_cast<const uint8_t*>(&hash),
                 reinterpret_cast<const uint8_t*>(&hash) + 8);
      all.insert(all.end(), s.source.begin(), s.source.end());
      all.insert(all.end(), s.container.begin(), s.container.end());
    }
    std::printf("scan digest: %016llx\n",
                (unsigned long long)mantra::shader_key::ExactHash(all.data(), all.size()));
    for (const auto& dir : dumps) ReportDumpCoverage(dir, shaders, true);
    return 0;
  }

  std::unordered_set<uint64_t> requested;  // exact hashes of the --dumps programs
  // Runtime-patched variants: the D3D runtime patches vertex shaders at draw
  // time (vertex fetch formats/offsets, export swizzles and masks for the
  // pixel shader's inputs). Each dumped program whose normalized hash matches
  // a game shader of the same size is translated from that shader's container
  // with the dumped microcode, and cached under its exact hash.
  if (!dumps.empty()) {
    // (In exact hash order: the first candidate is the one taken before.)
    std::unordered_map<uint64_t, std::vector<const Shader*>> by_normalized;
    for (const auto& [hash, s] : shaders) by_normalized[s.normalized_hash].push_back(&s);
    size_t variants = 0;
    std::vector<std::pair<uint64_t, Shader>> added;
    std::unordered_set<uint64_t> seen;  // the same dump can be in several folders
    for (const auto& dir : dumps)
    for (const auto& entry : fs::directory_iterator(dir)) {
      if (entry.path().extension() != ".mc") continue;
      const auto code = ReadFile(entry.path());
      const uint64_t exact = mantra::shader_key::ExactHash(code.data(), code.size());
      requested.insert(exact);
      if (shaders.count(exact) || !seen.insert(exact).second) continue;
      const bool is_pixel = entry.path().filename().string().rfind("ps", 0) == 0;
      // Several game shaders can share a normalized hash; take one that
      // declares every interpolator the patched code exports to.
      const int max_export = is_pixel ? -1 : MaxInterpolatorExport(code);
      const Shader* found = nullptr;
      const auto candidates =
          by_normalized.find(mantra::shader_key::NormalizedHash(code.data(), code.size()));
      if (candidates == by_normalized.end()) continue;
      for (const Shader* candidate_ptr : candidates->second) {
        const Shader& candidate = *candidate_ptr;
        if (candidate.is_pixel != is_pixel || candidate.microcode_dwords * 4 != code.size()) continue;
        if (!found) found = &candidate;
        if (is_pixel || int(ContainerInterpolatorCount(candidate.container)) > max_export) {
          found = &candidate;
          break;
        }
      }
      if (!found) continue;
      if (!is_pixel && int(ContainerInterpolatorCount(found->container)) <= max_export) {
        std::fprintf(stderr, "%s exports o%d, past its container's %u interpolators\n",
                     entry.path().filename().string().c_str(), max_export,
                     ContainerInterpolatorCount(found->container));
      }
      const Shader& original = *found;
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

  if (update) {
    const int taken = ReadExistingCache(out, shaders);
    if (taken < 0) {
      std::printf("update: %s can't be reused (missing or another version), translating all\n",
                  out.string().c_str());
    } else {
      std::printf("update: %d shaders from the existing cache, %zu new\n", taken,
                  shaders.size() - size_t(taken));
    }
  }

  // A stopped full build left its progress in <out>.partial: take it, so
  // only the rest is translated. Not for --requested-only (a few shaders
  // next to the game), which leaves the file for the full build.
  const fs::path partial = out.string() + ".partial";
  size_t resumed = 0;
  if (!requested_only && only.empty()) {
    std::error_code ec;
    if (fresh) {
      fs::remove(partial, ec);
    } else if (const int taken = ReadExistingCache(partial, shaders); taken > 0) {
      for (const auto& [hash, s] : shaders) resumed += s.from_cache;
      std::printf("resume: %d shaders from the stopped build\n", taken);
    }
  }
  // --update shaders came from the finished cache: not part of this build's count.
  if (update) resumed = 0;

  std::vector<Shader*> list;
  for (auto it = shaders.begin(); it != shaders.end();) {
    if (!only.empty() && !only.count(it->first)) {
      it = shaders.erase(it);
      continue;
    }
    if (requested_only && !it->second.from_cache && !requested.count(it->first)) {
      it = shaders.erase(it);  // not used by the game yet: for a full build
      continue;
    }
    if (!it->second.from_cache) list.push_back(&it->second);
    ++it;
  }
  const fs::path work = fs::temp_directory_path() / ("mantra_shadergen_" + std::to_string(asura::platform::ProcessId()));
  fs::create_directories(work);
  if (!hlsl_dir.empty()) {
    fs::create_directories(hlsl_dir);
  }
  std::printf("translating with %u jobs...\n", jobs);
  auto checkpoint = [&] {
    if (requested_only || !only.empty()) return;
    if (!WriteCache(partial, shaders, /*tried_only=*/true)) {
      std::fprintf(stderr, "could not write %s\n", partial.string().c_str());
    }
  };
  const bool finished = TranslateAll(list, work, hlsl_dir, jobs, limits, resumed, checkpoint);
  if (keep_work) {
    std::printf("kept %s\n", work.string().c_str());
  } else {
    fs::remove_all(work);
  }
  if (!finished) {
    size_t tried = 0;
    for (const auto& [hash, s] : shaders) tried += s.attempted || s.from_cache;
    std::printf("stopped: %zu of %zu shaders done, kept in %s (the next build continues)\n",
                tried, shaders.size(), partial.string().c_str());
    std::error_code ec;
    fs::remove(g_stop_file, ec);
    return kExitStopped;
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
    std::fprintf(stderr, "failed to write %s\n", out.string().c_str());
    return 1;
  }
  std::printf("wrote %s\n", out.string().c_str());
  if (!requested_only && only.empty()) {  // a complete build
    std::error_code ec;
    fs::remove(partial, ec);
  }
  for (const auto& dir : dumps) {
    ReportDumpCoverage(dir, shaders);
  }
  return 0;
}
