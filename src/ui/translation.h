#pragma once
// Fan translations the launcher offers as languages of their own (Settings >
// Language), shipped in assets/translations/<tag> (tools/make_ptbr_pack.py):
//   - Português (Brasil), language=9 in launcher.cfg (XLanguage 9 is
//     Portuguese, which the game doesn't have): the PT-BR translation by
//     XEX Brasil / Brazilian Warriors (joao13traducoes.com). It replaces the
//     Spanish files, so the game runs in Spanish with those files in place of
//     its own (asura::game::ApplyTranslation).
// A translation folder holds game files at their paths in the game folder
// (BCGame/CookedXbox360/..., BCGame/Movies/...), achievements.tsv (id, name,
// description) and CREDITS.txt.

#include <cstdint>
#include <filesystem>
#include <string>

namespace asura::ui {

inline constexpr uint32_t kLanguagePortugueseBrazil = 9;

// Whether `language` (launcher.cfg's language=) is one of the translations.
bool IsTranslation(uint32_t language);
// The XLanguage the game itself runs in for `language`: the one whose files a
// translation replaces, else `language`.
uint32_t GameLanguage(uint32_t language);
// The translation's folder (<exe dir>/assets/translations/<tag>), empty if
// `language` isn't one or it isn't installed.
std::filesystem::path TranslationDir(uint32_t language);
// A game file (relative to the game folder, "BCGame/CookedXbox360/...") as
// the game sees it in `language`: the translation's copy if it has one, else
// the game's own under `game_root`.
std::filesystem::path GameFileFor(const std::filesystem::path& game_root, uint32_t language,
                                  const std::filesystem::path& relative);
// Achievement `id`'s name and description in the translation (both empty if
// `language` isn't one or the list lacks it).
struct TranslatedAchievement {
  std::string name, description;
};
TranslatedAchievement TranslatedAchievementText(uint32_t language, uint32_t id);

}  // namespace asura::ui
