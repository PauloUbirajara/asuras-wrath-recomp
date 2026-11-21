// A fan translation (Settings > Language, src/ui/translation.h): its game
// files take the place of the game's own of the same name
// (ReplaceGameFiles, game_files.h), one device per game folder. The game
// itself runs in the language whose files they replace (GameLanguage).
#pragma once

#include <cstdint>

namespace rex {
class Runtime;
}

namespace asura::game {

// Call once the VFS is set up and before the game starts (OnPreLaunchModule),
// before the other replacements (skip the intro, graphics effects), which then
// replace the translated files. `language`: launcher.cfg's language=.
void ApplyTranslation(rex::Runtime* runtime, uint32_t language);

}  // namespace asura::game
