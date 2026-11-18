#pragma once
// The launcher's text in the game's languages (Settings > Language). Strings
// are looked up by their English text; one without a translation shows in
// English. Translations live in launcher_text.cpp.

#include <cstdint>

namespace asura::ui {

// XLanguage: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian.
void SetLauncherLanguage(uint32_t language);
uint32_t LauncherLanguage();

// `english` in the launcher's language. Format strings keep their
// specifiers in the same order.
const char* T(const char* english);

}  // namespace asura::ui
