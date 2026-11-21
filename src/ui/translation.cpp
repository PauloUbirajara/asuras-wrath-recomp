#include "translation.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>

#include "../platform/platform.h"

namespace asura::ui {

namespace fs = std::filesystem;

bool IsTranslation(uint32_t language) { return language == kLanguagePortugueseBrazil; }

uint32_t GameLanguage(uint32_t language) {
  return language == kLanguagePortugueseBrazil ? 5 : language;  // Spanish
}

fs::path TranslationDir(uint32_t language) {
  if (language != kLanguagePortugueseBrazil) return {};
  // Debug: another copy of the translation's folder (tests of its files).
  if (const char* dir = std::getenv("ASURA_TRANSLATION_DIR"); dir && *dir) return fs::path(dir);
  const fs::path exe = asura::platform::ExePath();
  if (exe.empty()) return {};
  const fs::path dir = exe.parent_path() / "assets" / "translations" / "pt-BR";
  std::error_code ec;
  return fs::is_directory(dir / "BCGame", ec) ? dir : fs::path();
}

fs::path GameFileFor(const fs::path& game_root, uint32_t language, const fs::path& relative) {
  const fs::path dir = TranslationDir(language);
  std::error_code ec;
  if (!dir.empty() && fs::is_regular_file(dir / relative, ec)) return dir / relative;
  return game_root / relative;
}

TranslatedAchievement TranslatedAchievementText(uint32_t language, uint32_t id) {
  static std::mutex mutex;
  static std::map<uint32_t, std::map<uint32_t, TranslatedAchievement>> lists;
  std::lock_guard lock(mutex);
  auto it = lists.find(language);
  if (it == lists.end()) {
    std::map<uint32_t, TranslatedAchievement> list;
    if (const fs::path dir = TranslationDir(language); !dir.empty()) {
      std::ifstream in(dir / "achievements.tsv");
      std::string line;
      std::getline(in, line);  // header
      while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t a = line.find('\t');
        const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        list[uint32_t(std::strtoul(line.c_str(), nullptr, 10))] = {line.substr(a + 1, b - a - 1),
                                                                    line.substr(b + 1)};
      }
    }
    it = lists.emplace(language, std::move(list)).first;
  }
  const auto found = it->second.find(id);
  return found == it->second.end() ? TranslatedAchievement{} : found->second;
}

}  // namespace asura::ui
