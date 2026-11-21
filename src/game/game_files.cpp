#include "game_files.h"

#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <utility>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/string/utf8.h>

#include "../ui/game_assets.h"

namespace asura::game {
namespace {

using rex::filesystem::Entry;

// An entry's child list (Entry::children_ is protected; a member pointer
// named through a derived class reaches it).
struct ChildrenOf : Entry {
  static std::vector<std::unique_ptr<Entry>>& Get(Entry* entry) {
    return entry->*(&ChildrenOf::children_);
  }
};

std::unique_ptr<Entry>* FindChild(Entry* folder, const std::string& name) {
  for (auto& child : ChildrenOf::Get(folder)) {
    if (child && rex::string::utf8_equal_case(child->name(), name)) return &child;
  }
  return nullptr;
}

std::string Lower(std::string text) {
  for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// Sizes of the copies now in place, by lowercase path relative to d:
// ("bcgame\\movies\\x.bik"); a later copy of the same file wins.
std::map<std::string, uintmax_t>& ReplacedSizes() {
  static std::map<std::string, uintmax_t> sizes;
  return sizes;
}

}  // namespace

size_t ReplaceGameFiles(rex::Runtime* runtime, const char* device,
                        const std::filesystem::path& host_folder, const std::string& game_folder,
                        const std::vector<std::string>& names) {
  auto* vfs = runtime ? runtime->file_system() : nullptr;
  if (!vfs || names.empty()) return 0;
  std::string root;
  if (!vfs->FindSymbolicLink("d:", root)) {
    REXLOG_WARN("{}: d: isn't mounted", device);
    return 0;
  }
  Entry* folder = vfs->ResolvePath(root + "\\" + game_folder);
  if (!folder) {
    REXLOG_WARN("{}: no {} in the game folder", device, game_folder);
    return 0;
  }
  auto mounted = std::make_unique<rex::filesystem::HostPathDevice>(device, host_folder, true);
  if (!mounted->Initialize() || !vfs->RegisterDevice(std::move(mounted))) {
    REXLOG_WARN("{}: cannot mount {}", device, host_folder.string());
    return 0;
  }
  Entry* ours = vfs->ResolvePath(device);
  if (!ours) return 0;
  size_t replaced = 0;
  for (const auto& name : names) {
    std::unique_ptr<Entry>* copy = FindChild(ours, name);
    std::unique_ptr<Entry>* original = FindChild(folder, name);
    if (!copy || !original) {
      REXLOG_WARN("{}: {} missing in {}", device, name, copy ? game_folder : host_folder.string());
      continue;
    }
    // Each entry keeps its own device and host path: the game's folder now
    // opens the copy, and our device holds the original (never opened).
    std::swap(*copy, *original);
    ++replaced;
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(host_folder / name, ec);
    if (!ec) ReplacedSizes()[Lower(game_folder + "\\" + name)] = size;
  }
  return replaced;
}

void FixGameToc(rex::Runtime* runtime) {
  namespace fs = std::filesystem;
  if (!runtime || ReplacedSizes().empty()) return;
  const fs::path toc_path = runtime->game_data_root() / "BCGame" / "Xbox360TOC.txt";
  std::ifstream in(toc_path, std::ios::binary);
  if (!in) return;
  std::stringstream text;
  text << in.rdbuf();
  // Lines: "<size> <uncompressed size> ..\\<path from d:> <checksum>", CRLF.
  std::string out, line;
  size_t fixed = 0;
  std::istringstream lines(text.str());
  while (std::getline(lines, line)) {
    const bool cr = !line.empty() && line.back() == '\r';
    if (cr) line.pop_back();
    std::istringstream fields(line);
    std::string size, uncompressed, path, rest;
    if (fields >> size >> uncompressed >> path) {
      std::getline(fields, rest);
      const std::string key = Lower(path.rfind("..\\", 0) == 0 ? path.substr(3) : path);
      const auto it = ReplacedSizes().find(key);
      if (it != ReplacedSizes().end() && std::to_string(it->second) != size) {
        line = std::to_string(it->second) + " 0 " + path + rest;
        ++fixed;
      }
    }
    out += line;
    if (cr) out += '\r';
    out += '\n';
  }
  if (!fixed) return;
  const fs::path cache = asura::ui::GameCacheRoot();
  if (cache.empty()) return;
  const fs::path folder = cache / "toc";
  std::error_code ec;
  fs::create_directories(folder, ec);
  {
    std::ofstream file(folder / "Xbox360TOC.txt", std::ios::binary | std::ios::trunc);
    file << out;
    if (!file) {
      REXLOG_WARN("TOC: cannot write {}", (folder / "Xbox360TOC.txt").string());
      return;
    }
  }
  if (ReplaceGameFiles(runtime, "\\Device\\AsuraToc", folder, "BCGame", {"Xbox360TOC.txt"})) {
    REXLOG_INFO("TOC: {} file sizes updated for the replaced files", fixed);
  }
}

}  // namespace asura::game
