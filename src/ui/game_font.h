#pragma once
// Asura's Wrath bitmap UI fonts (.awf) for ImGui overlays.
//
// The .awf files come from the game's UE3 bitmap fonts (UI_Fonts.TitleFont /
// MediumFont, FOT-Matisse Pro): extracted at startup from the player's game
// folder into the user's cache (ui/game_assets.h), or offline by
// tools/asset_extract.py (see that script for the format). They are game
// assets, never shipped.

#include <filesystem>
#include <vector>
#include <string_view>

struct ImFont;
struct ImFontAtlas;

namespace asura::ui {

// UI file lookup (.awf fonts, toast images), first match per file:
// $ASURA_UI_DIR, $XDG_CACHE_HOME/asura_wrath_recomp/ui, ~/.cache/asura_wrath_recomp/ui,
// the repository's out/assets/ui (extracted game files), then <exe dir>/assets/ui
// (the project's own art from assets/ui, copied there at build time). Earlier
// directories override later ones, so a cache copy replaces the shipped art.
std::vector<std::filesystem::path> GameUiDirs();
std::filesystem::path GameUiFile(std::string_view name);

// Extra copies of each font extracted from the localized packages
// (<name>_<lang>.awf), merged in for the glyphs the international one lacks.
inline constexpr const char* kGameFontLanguages[] = {"ITA", "ESN", "JPN"};

// Adds an .awf font to `atlas` through a custom ImFontLoader (glyphs are
// resampled from the native bitmap to each requested size), or merges it into
// `merge_into`. Returns nullptr if the file is missing or invalid.
ImFont* AddGameFont(ImFontAtlas* atlas, const std::filesystem::path& path,
                    ImFont* merge_into = nullptr);
// Merges Noto Sans JP (when shipped in assets/ui) and, with `imgui_font`,
// ImGui's default font into `font` for the glyphs it lacks.
void AddFallbackFonts(ImFontAtlas* atlas, ImFont* font, bool imgui_font = true);
// "MediumFont" / "TitleFont": the international font, its localized copies
// and the fallbacks.
ImFont* AddGameFontFamily(ImFontAtlas* atlas, std::string_view name);

}  // namespace asura::ui
