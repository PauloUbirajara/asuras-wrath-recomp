// Skip the intro (Graphics > Enhancements > Skip the logos at start).
//
// At start the game plays D:\BCGame\Movies\00_AutoSave_Xenon*.bik (the
// autosave notice), 40_CAPCOMLogo, 40_CC2Logo, 20_UE3Logo and
// 20_SpeedTreeLogo before the title. The names are built in the executable
// (not in Coalesced's StartupMovies), so the movies are swapped instead for
// copies cut to their first (black) frame. Hiding them doesn't work: the
// start waits forever for a missing movie.
//
// The SDK's VFS applies symbolic links to a file's folder only (OpenFile
// resolves the parent, then looks the name up in it), so a link per file
// does nothing. Instead the game's Movies folder is redirected to a folder in
// the cache holding host symlinks to every other movie, mounted as a
// device of its own, with the cut copies in place of the logos.
#pragma once

namespace rex {
class Runtime;
}

namespace asura::game {

// Call once the VFS is set up and before the game opens the movies.
void SkipIntroMovies(rex::Runtime* runtime);

}  // namespace asura::game
