// Skip the intro (Graphics > Enhancements > Skip the logos at start).
//
// At start the game plays D:\BCGame\Movies\00_AutoSave_Xenon*.bik (the
// autosave notice), 40_CAPCOMLogo, 40_CC2Logo, 20_UE3Logo and
// 20_SpeedTreeLogo before the title. The names are built in the executable
// (not in Coalesced's StartupMovies), so the movies are swapped instead for
// copies cut to their first (black) frame. Hiding them doesn't work: the
// start waits forever for a missing movie.
//
// Only the cut copies are written (to <cache>/movies); each takes the place
// of the game's movie of the same name (ReplaceGameFiles, game_files.h).
#pragma once

namespace rex {
class Runtime;
}

namespace asura::game {

// Call once the VFS is set up and before the game starts (OnPreLaunchModule).
void SkipIntroMovies(rex::Runtime* runtime);

}  // namespace asura::game
