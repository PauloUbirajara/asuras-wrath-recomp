#pragma once
// Controller button remapping, set from the launcher (Controls > Controller).
//
// The game's input system is built by CreateInputSystem (passed to the SDK in
// OnPreSetup): the SDK's own drivers, with the SDL gamepad driver wrapped so
// its pads go through the remap. The keyboard has its own bindings
// (keybind_* flags) and is not remapped. Stick deadzones are the SDK's
// left/right_stick_deadzone_percentage flags.

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

#include <rex/input/input.h>

namespace rex::system {
class IInputSystem;
}

namespace asura::input {

// Pad controls in the Xbox layout (what the SDL driver reports).
enum PadControl : uint8_t {
  kPadA, kPadB, kPadX, kPadY,
  kPadLB, kPadRB, kPadLT, kPadRT,
  kPadBack, kPadStart, kPadLS, kPadRS,
  kPadUp, kPadDown, kPadLeft, kPadRight,
  kPadControlCount
};

// source[target]: the physical control that acts as `target`.
using PadMap = std::array<uint8_t, kPadControlCount>;

PadMap IdentityPadMap();
// Short config names ("a", "lb", "lt", "back", "up", ...).
const char* PadControlName(int control);
int PadControlFromName(std::string_view name);  // -1 if unknown

void SetPadMap(const PadMap& map);
// Controller vibration strength in percent (0 = off, 100 = as the game asks).
void SetVibrationStrength(int percent);
// Turbo: while a control in `mask` (bit = PadControl, after the remap) is
// held, the game sees it pressed and released about 12 times a second.
void SetTurboControls(uint32_t mask);
PadMap GetPadMap();

// Whether `control` is down on `pad` (triggers past the XInput threshold).
bool PadDown(const rex::input::X_INPUT_GAMEPAD& pad, int control);
// Rewrites the buttons and triggers of `pad` through `map`.
void RemapPad(rex::input::X_INPUT_GAMEPAD& pad, const PadMap& map);

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode);

}  // namespace asura::input
