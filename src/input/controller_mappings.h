// Controller mappings made with Controls > Setup (ui/controller_setup.h): one
// SDL gamepad mapping per controller model (its GUID), saved next to
// launcher.cfg as controller_mappings.txt (gamecontrollerdb.txt's format).
// SDL reads the file whenever its gamepad subsystem starts (the launcher's,
// the game's: SDL_HINT_GAMECONTROLLERCONFIG_FILE), and a saved mapping
// applies at once to the pads already open.
#pragma once

#include <filesystem>
#include <string>

namespace asura::input {

// Where the mappings live; call before SDL's gamepad subsystem starts.
void SetControllerMappingsFile(const std::filesystem::path& file);
std::filesystem::path ControllerMappingsFile();

// The saved mapping of a controller model (its SDL GUID string), or "".
std::string SavedControllerMapping(const std::string& guid);
// Saves `mapping` ("<guid>,<name>,a:b0,...") in place of the model's
// previous one and applies it. False if the file can't be written.
bool SaveControllerMapping(const std::string& guid, const std::string& mapping);
// Forgets the model's mapping (SDL's own applies from the next start).
bool RemoveControllerMapping(const std::string& guid);

// Android: Controls › Controller setup (ControllerSetup.kt, in the app's own
// menu) saves its mappings beside launcher.cfg as `file`, one controller
// model a line: "<vendor>,<product>,<name>,<SDL mapping fields>" (the app
// can't know SDL's GUID, so the game matches the model and adds the mapping
// under its GUID). Applied to the controllers connected now and to each one
// connected later; call again after the menu (the setup may have changed).
void ApplyAndroidControllerSetup(const std::filesystem::path& file);

}  // namespace asura::input
