// Controls > Setup: maps a controller SDL reads wrongly. Each control is a
// row (A, B, ..., Left stick up, ...) with the raw joystick input SDL sees
// for it (button, hat or axis half); Set listens for the next one pressed
// (10 s, as the keyboard's bindings), and Start setup goes through them all
// in turn. Every answer is saved at once as an SDL gamepad mapping for that
// controller model (input/controller_mappings.h), used by the launcher and
// the game alike.
// Desktop only: Android has its own (ControllerSetup.kt, from Android's input
// events), as the game's menu there is the app's.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL_joystick.h>

namespace asura::ui {

class ControllerSetup {
 public:
  ControllerSetup();
  ~ControllerSetup();
  ControllerSetup(const ControllerSetup&) = delete;
  ControllerSetup& operator=(const ControllerSetup&) = delete;

  // The tab's contents.
  void Draw(float scale);
  // Escape / Back: stops listening. True if it was.
  bool Back();
  bool Listening() const { return listening_ >= 0; }

  static constexpr int kSteps = 24;
  static constexpr int kListenSeconds = 10;

 private:
  void Open(SDL_JoystickID id);
  void Close();
  void Load();
  void RecordRest();
  void Listen(int step, bool sequence);
  void Capture();
  bool Released() const;
  void Set(int step, const std::string& input);
  std::string Mapping() const;

  bool subsystem_ = false;
  SDL_JoystickID chosen_ = 0;
  SDL_Joystick* joystick_ = nullptr;
  std::string guid_;
  std::string name_;
  bool saved_ = false;                      // controller_mappings.txt has this model
  std::vector<int16_t> rest_;               // axes at rest
  std::array<std::string, kSteps> inputs_;  // per step: "b0", "h0.1", "+a2", "a5", "" unset
  std::string extra_;                       // the mapping's other fields (guide, platform, ...)
  int listening_ = -1;                      // the step listened for
  bool sequence_ = false;                   // Start setup: then the next step
  uint64_t listen_since_ = 0;
  bool armed_ = false;                      // everything let go since listening began
  std::string pending_button_;              // a trigger's button, kept briefly for an axis
  uint64_t pending_since_ = 0;
  std::string message_;
};

}  // namespace asura::ui
