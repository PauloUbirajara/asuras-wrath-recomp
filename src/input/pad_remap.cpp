#include "pad_remap.h"

#include <atomic>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>

namespace asura::input {

using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;

namespace {

constexpr const char* kNames[kPadControlCount] = {
    "a",    "b",     "x",  "y",  "lb", "rb",   "lt",   "rt",
    "back", "start", "ls", "rs", "up", "down", "left", "right",
};

// Button bit per control (0 for the triggers).
constexpr uint16_t kBits[kPadControlCount] = {
    X_INPUT_GAMEPAD_A,         X_INPUT_GAMEPAD_B,
    X_INPUT_GAMEPAD_X,         X_INPUT_GAMEPAD_Y,
    X_INPUT_GAMEPAD_LEFT_SHOULDER, X_INPUT_GAMEPAD_RIGHT_SHOULDER,
    0,                         0,
    X_INPUT_GAMEPAD_BACK,      X_INPUT_GAMEPAD_START,
    X_INPUT_GAMEPAD_LEFT_THUMB, X_INPUT_GAMEPAD_RIGHT_THUMB,
    X_INPUT_GAMEPAD_DPAD_UP,   X_INPUT_GAMEPAD_DPAD_DOWN,
    X_INPUT_GAMEPAD_DPAD_LEFT, X_INPUT_GAMEPAD_DPAD_RIGHT,
};

// 16 controls x 4 bits.
uint64_t Pack(const PadMap& map) {
  uint64_t packed = 0;
  for (int i = 0; i < kPadControlCount; ++i) packed |= uint64_t(map[i] & 0xF) << (i * 4);
  return packed;
}

PadMap Unpack(uint64_t packed) {
  PadMap map;
  for (int i = 0; i < kPadControlCount; ++i) map[i] = uint8_t((packed >> (i * 4)) & 0xF);
  return map;
}

std::atomic<uint64_t> g_map{Pack(IdentityPadMap())};

uint8_t Value(const X_INPUT_GAMEPAD& pad, int control) {
  if (control == kPadLT) return pad.left_trigger;
  if (control == kPadRT) return pad.right_trigger;
  return (uint16_t(pad.buttons) & kBits[control]) ? 255 : 0;
}

// The SDK's SDL driver with its pads' buttons remapped.
class RemappedPadDriver final : public InputDriver {
 public:
  RemappedPadDriver() : InputDriver(nullptr, 0), inner_(std::make_unique<sdl::SDLInputDriver>(nullptr, 0)) {
    // The SDK sets the focus callback on the drivers it holds (this one).
    inner_->set_is_active_callback([this] { return is_active(); });
  }
  X_STATUS Setup() override { return inner_->Setup(); }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override { inner_->EnumerateDevices(out); }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    const X_RESULT result = inner_->GetDeviceState(id, out_state);
    if (result == X_ERROR_SUCCESS && out_state) {
      const PadMap map = GetPadMap();
      if (map != IdentityPadMap()) RemapPad(out_state->gamepad, map);
    }
    return result;
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES* out) override {
    return inner_->GetDeviceCapabilities(id, flags, out);
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override {
    return inner_->SetDeviceVibration(id, vibration);
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags, X_INPUT_KEYSTROKE* out) override {
    return inner_->GetDeviceKeystroke(id, flags, out);
  }
  void OnWindowAvailable(rex::ui::Window* window) override { inner_->OnWindowAvailable(window); }

 private:
  std::unique_ptr<sdl::SDLInputDriver> inner_;
};

}  // namespace

PadMap IdentityPadMap() {
  PadMap map;
  for (int i = 0; i < kPadControlCount; ++i) map[i] = uint8_t(i);
  return map;
}

const char* PadControlName(int control) {
  return control >= 0 && control < kPadControlCount ? kNames[control] : "";
}

int PadControlFromName(std::string_view name) {
  for (int i = 0; i < kPadControlCount; ++i) {
    if (name == kNames[i]) return i;
  }
  return -1;
}

void SetPadMap(const PadMap& map) { g_map = Pack(map); }
PadMap GetPadMap() { return Unpack(g_map); }

bool PadDown(const X_INPUT_GAMEPAD& pad, int control) {
  return Value(pad, control) > X_INPUT_GAMEPAD_TRIGGER_THRESHOLD;
}

void RemapPad(X_INPUT_GAMEPAD& pad, const PadMap& map) {
  const X_INPUT_GAMEPAD in = pad;
  uint16_t buttons = uint16_t(in.buttons) & X_INPUT_GAMEPAD_GUIDE;
  uint8_t lt = 0, rt = 0;
  for (int target = 0; target < kPadControlCount; ++target) {
    const uint8_t value = Value(in, map[target]);
    if (target == kPadLT) {
      lt = value;
    } else if (target == kPadRT) {
      rt = value;
    } else if (value > X_INPUT_GAMEPAD_TRIGGER_THRESHOLD) {
      buttons |= kBits[target];
    }
  }
  pad.buttons = buttons;
  pad.left_trigger = lt;
  pad.right_trigger = rt;
}

// CreateDefaultInputSystem (input_system.cpp) with the SDL driver wrapped.
std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);
  if (!tool_mode) {
    if (REXCVAR_GET(input_backend) == "sdl") {
      auto pads = std::make_unique<RemappedPadDriver>();
      if (pads->Setup() == X_STATUS_SUCCESS) input->AddDriver(std::move(pads));
    }
    auto mnk = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk->Setup() == X_STATUS_SUCCESS) input->AddDriver(std::move(mnk));
  }
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, tool_mode ? 0 : 1));
  input->SetDeviceAssignment(std::make_unique<SlotAssignment>());
  return input;
}

}  // namespace asura::input
