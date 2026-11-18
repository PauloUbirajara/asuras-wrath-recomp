#pragma once
// Asura's Wrath bitmap UI fonts (.awf) for ImGui overlays.
//
// The .awf files are made offline from the game's UE3 bitmap fonts
// (UI_Fonts.TitleFont / MediumFont, FOT-Matisse Pro) by
// tools/asset_extract.py; see that script for the format. They are game
// assets: they are read from the user's cache, never shipped.

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

// Adds an .awf font to `atlas` through a custom ImFontLoader (glyphs are
// resampled from the native bitmap to each requested size). Returns nullptr
// if the file is missing or invalid.
ImFont* AddGameFont(ImFontAtlas* atlas, const std::filesystem::path& path);

}  // namespace asura::ui
