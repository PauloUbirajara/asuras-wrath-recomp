#include "game_files.h"

#include <memory>
#include <utility>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/string/utf8.h>

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
  }
  return replaced;
}

}  // namespace asura::game
