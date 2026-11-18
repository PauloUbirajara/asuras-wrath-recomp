#pragma once
// Game files the launcher and the toast use, extracted from the player's own
// copy of the game into the user's cache (never shipped):
//   - the UI fonts (UI_Fonts.MediumFont / TitleFont in Startup_LOC_INT.xxx)
//     as .awf files (format in tools/asset_extract.py, read by game_font.cpp);
//   - the achievement list and icons, from the title's XDBF resource inside
//     the encrypted default.xex.

#include <cstdint>
#include <filesystem>
#include <string>

namespace asura::ui {

// <XDG cache>/asura_wrath_recomp ("" without HOME / XDG_CACHE_HOME).
std::filesystem::path GameCacheRoot();
// Where extracted UI files go (<cache>/ui, searched by GameUiFile).
std::filesystem::path UiCacheDir();
// Where the achievement list goes (<cache>/achievements):
//   achievements.tsv  id, gamerscore, name, description, locked_description
//   <id>.png          icons
//   language          the XLanguage the strings are in
std::filesystem::path AchievementCacheDir();

// MediumFont.awf and TitleFont.awf into `out_dir`, with the localized copies
// (<name>_<lang>.awf, kGameFontLanguages) and fonts.version.
bool ExtractUiFonts(const std::filesystem::path& game_dir, const std::filesystem::path& out_dir,
                    std::string* error);
// Whether UiCacheDir() has this version's fonts (bump to re-extract).
inline constexpr const char* kUiFontsVersion = "2";
bool HaveExtractedUiFonts();

// The achievement list in `language` (XLanguage; the game's default if it
// lacks that one) into `out_dir`.
bool ExtractAchievementList(const std::filesystem::path& game_dir,
                            const std::filesystem::path& out_dir, uint32_t language,
                            std::string* error);

// The language the cached list is in, 0 if there is none.
uint32_t CachedAchievementLanguage();

}  // namespace asura::ui
