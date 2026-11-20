#include "skip_intro.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "../platform/platform.h"
#include "../ui/game_assets.h"
#include "game_files.h"

namespace asura::game {
namespace {

constexpr const char* kDevice = "\\Device\\AsuraMovies";

// Prefixes: the autosave notice has a file per language.
bool Skipped(const std::string& name) {
  static const char* kMovies[] = {"00_AutoSave_Xenon", "40_CAPCOMLogo",    "40_CC2Logo",
                                  "20_UE3Logo",        "20_SpeedTreeLogo", "20_BINKLogo"};
  for (const char* prefix : kMovies) {
    if (name.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

// A copy of a Bink 1 movie cut to its first frame (black in these), so the
// player finishes it at once. Header: "BIK?", u32 file size - 8, u32 frames,
// u32 largest frame, u32 frames, width, height, fps, fps, flags, audio track
// count, 12 bytes per audio track, then frames + 1 u32 frame offsets (bit 0 =
// key frame) and the frame data.
bool WriteFirstFrameOnly(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::ifstream in(from, std::ios::binary);
  const std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), {});
  auto u32 = [&](size_t at) {
    uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
  };
  if (d.size() < 44 || std::memcmp(d.data(), "BIK", 3) != 0) return false;
  const uint32_t frames = u32(8), audio = u32(40);
  const size_t index = 44 + size_t(audio) * 12;
  if (frames < 1 || index + (size_t(frames) + 1) * 4 > d.size()) return false;
  const uint32_t first = u32(index) & ~1u, second = u32(index + 4) & ~1u;
  if (first > second || second > d.size()) return false;
  const uint32_t frame_size = second - first;
  std::vector<uint8_t> out(d.begin(), d.begin() + index);
  const uint32_t start = uint32_t(index + 8);
  auto put = [&](size_t at, uint32_t v) { std::memcpy(out.data() + at, &v, 4); };
  out.resize(index + 8);
  put(index, start | (u32(index) & 1u));
  put(index + 4, start + frame_size);
  out.insert(out.end(), d.begin() + first, d.begin() + second);
  put(4, uint32_t(out.size() - 8));
  put(8, 1);
  put(12, frame_size);
  put(16, 1);
  std::ofstream file(to, std::ios::binary);
  file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  return bool(file);
}

}  // namespace

void SkipIntroMovies(rex::Runtime* runtime) {
  namespace fs = std::filesystem;
  auto* vfs = runtime ? runtime->file_system() : nullptr;
  if (!vfs) return;
  const fs::path movies = runtime->game_data_root() / "BCGame" / "Movies";
  const fs::path cache = asura::ui::GameCacheRoot();
  std::error_code ec;
  if (cache.empty() || !fs::is_directory(movies, ec)) {
    REXLOG_WARN("Skip the intro: no {} or cache folder", movies.string());
    return;
  }
  // Rebuilt each start: the game folder may have changed. Only the cut
  // movies go in it; each takes the place of the game's own (game_files.h),
  // so the rest of Movies stays the game's and nothing needs a host symlink.
  const fs::path folder = cache / "movies";
  fs::remove_all(folder, ec);
  fs::create_directories(folder, ec);
  std::vector<std::string> cut;
  for (const auto& entry : fs::directory_iterator(movies, ec)) {
    const std::string name = entry.path().filename().string();
    if (!Skipped(name) || entry.path().extension() != ".bik") continue;
    // A missing movie stalls the start (the player waits for it).
    if (WriteFirstFrameOnly(entry.path(), folder / name)) {
      cut.push_back(name);
    } else {
      REXLOG_WARN("Skip the intro: cannot shorten {}, keeping it", name);
    }
  }
  if (cut.empty()) {
    REXLOG_WARN("Skip the intro: no startup movies found in {}", movies.string());
    return;
  }
  const size_t replaced = ReplaceGameFiles(runtime, kDevice, folder, "BCGame\\Movies", cut);
  if (!replaced) return;
  REXLOG_INFO("Skip the intro: {} startup movies cut to one frame (in {})", replaced, folder.string());
}

}  // namespace asura::game
