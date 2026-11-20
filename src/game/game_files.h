// Gives the game other copies of some of its files, one file at a time.
//
// The SDK's VFS opens a file by resolving its folder, then looking the name
// up among the folder's entries, so a symbolic link to a single file is never
// used. Instead the folder's entry for the file is swapped with the entry of
// the copy (in a folder of ours, mounted as a device): the game's listing then
// holds the copy's entry, which opens the copy. No host symlinks are needed
// (Android's shared storage has none).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rex {
class Runtime;
}

namespace asura::game {

// Mounts `host_folder` as `device` ("\\Device\\Asura...") and puts each of
// `names` (files in host_folder) in place of the game's file of the same name
// in `game_folder` (relative to d:, e.g. "BCGame\\Movies"). Returns how many
// were replaced. Call once per device, before the game opens the files.
size_t ReplaceGameFiles(rex::Runtime* runtime, const char* device,
                        const std::filesystem::path& host_folder, const std::string& game_folder,
                        const std::vector<std::string>& names);

}  // namespace asura::game
