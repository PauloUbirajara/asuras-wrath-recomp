#include "translation.h"

#include <filesystem>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>

#include "../ui/translation.h"
#include "game_files.h"

namespace asura::game {

void ApplyTranslation(rex::Runtime* runtime, uint32_t language) {
  namespace fs = std::filesystem;
  if (!runtime || !asura::ui::IsTranslation(language)) return;
  const fs::path dir = asura::ui::TranslationDir(language);
  if (dir.empty()) {
    REXLOG_WARN("Translation: no assets/translations folder next to the executable; the game "
                "stays in its own language");
    return;
  }
  // The folders the translation has files in, each mounted as a device.
  struct Folder {
    const char* device;
    const char* game_folder;  // relative to d:
    const char* host;         // relative to the translation's folder
  };
  static constexpr Folder kFolders[] = {
      {"\\Device\\AsuraTranslationCooked", "BCGame\\CookedXbox360", "BCGame/CookedXbox360"},
      {"\\Device\\AsuraTranslationMovies", "BCGame\\Movies", "BCGame/Movies"},
  };
  size_t replaced = 0;
  for (const Folder& folder : kFolders) {
    const fs::path host = dir / folder.host;
    std::error_code ec;
    if (!fs::is_directory(host, ec)) continue;
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(host, ec)) {
      if (entry.is_regular_file(ec)) names.push_back(entry.path().filename().string());
    }
    replaced += ReplaceGameFiles(runtime, folder.device, host, folder.game_folder, names);
  }
  REXLOG_INFO("Translation: {} game files from {}", replaced, dir.string());
}

}  // namespace asura::game
