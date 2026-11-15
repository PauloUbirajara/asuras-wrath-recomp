#pragma once
// Asura's Wrath bitmap UI fonts (.awf) for ImGui overlays.
//
// The .awf files are made offline from the game's UE3 bitmap fonts
// (UI_Fonts.TitleFont / MediumFont, FOT-Matisse Pro) by
// tools/asset_extract.py; see that script for the format. They are game
// assets: they are read from the user's cache, never shipped.

#include <filesystem>

struct ImFont;
struct ImFontAtlas;

namespace asura::ui {

// Directory of the extracted/user-made UI files (.awf fonts, toast images):
// $ASURA_UI_DIR, else the first existing of $XDG_CACHE_HOME/asura_wrath/ui,
// ~/.cache/asura_wrath/ui and the repository's out/assets/ui.
std::filesystem::path GameUiDir();

// Adds an .awf font to `atlas` through a custom ImFontLoader (glyphs are
// resampled from the native bitmap to each requested size). Returns nullptr
// if the file is missing or invalid.
ImFont* AddGameFont(ImFontAtlas* atlas, const std::filesystem::path& path);

}  // namespace asura::ui
