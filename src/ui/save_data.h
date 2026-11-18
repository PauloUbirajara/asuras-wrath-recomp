#pragma once
// Story progress from the game's save (SAVEDATA.DAT: UE3 tagged properties,
// big-endian) and the episode list from the game's own config
// (Coalesced_<LANG>.bin: BCUIDataProvider_ChapterInfo sections and the
// localized [Title] strings), for the launcher's Saves page.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace asura::ui {

enum Difficulty { kEasy, kNormal, kHard, kDifficultyCount };

struct StoryRecord {
  int id = 0;          // stage id: 0 = Episode 1 (BC-S000), 10 = Episode 2, ...
  int clear_flag = 0;  // 0 = not reached yet
  // Best total rank per difficulty ("S", "A", ...; empty = not cleared).
  std::array<std::string, kDifficultyCount> rank;
  bool Reached() const { return clear_flag != 0; }
  bool Cleared() const;
};

struct SaveSummary {
  std::vector<StoryRecord> stories;
  int difficulty = -1;  // the game's current setting, -1 = unknown
  int battle_points = 0, enemies_defeated = 0, continues = 0;
};

// False if the file can't be read or isn't a save.
bool ReadSaveSummary(const std::filesystem::path& file, SaveSummary* out);

struct EpisodeInfo {
  int id = 0;
  int part = 0;        // ChapterNo (Part I-III)
  std::string number;  // StoryNo: "1" ... "18_A", "18_B"
  std::string title;   // in the requested language (English if missing)
};

// The main story's episodes in order, from <game>/BCGame/CookedXbox360.
// `language` is an XLanguage (1 English ... 6 Italian). Empty if not found.
std::vector<EpisodeInfo> ReadEpisodes(const std::filesystem::path& game_root, uint32_t language);
// Title of any stage id ("S<id>" in [Title]), e.g. DLC episodes; empty if none.
std::string EpisodeTitle(const std::filesystem::path& game_root, uint32_t language, int id);

}  // namespace asura::ui
