#include "pad_remap.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

#ifdef __ANDROID__
#include <jni.h>
#endif

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
std::atomic<int> g_vibration{100};
std::atomic<uint32_t> g_turbo{0};

// Held turbo controls alternate between pressed and released from one read
// of the pad to the next (the game reads it once a frame), at most every
// 33 ms: so each phase spans at least one frame whatever the frame rate. A
// clock-based phase missed presses when frames were slower than the phase
// (low frame rates read the same phase several frames in a row).
std::atomic<int64_t> g_turbo_switched_ms{0};
std::atomic<bool> g_turbo_released{false};

void ApplyTurbo(X_INPUT_GAMEPAD& pad, uint32_t mask) {
  using namespace std::chrono;
  const int64_t ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  int64_t last = g_turbo_switched_ms.load(std::memory_order_relaxed);
  if (ms - last >= 33 && g_turbo_switched_ms.compare_exchange_strong(last, ms)) {
    g_turbo_released.store(!g_turbo_released.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
  }
  if (!g_turbo_released.load(std::memory_order_relaxed)) return;  // the "pressed" phase
  for (int c = 0; c < kPadControlCount; ++c) {
    if (!(mask & (1u << c))) continue;
    if (c == kPadLT) {
      pad.left_trigger = 0;
    } else if (c == kPadRT) {
      pad.right_trigger = 0;
    } else {
      pad.buttons = uint16_t(pad.buttons) & ~kBits[c];
    }
  }
}

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
      if (const uint32_t turbo = g_turbo.load(std::memory_order_relaxed)) {
        ApplyTurbo(out_state->gamepad, turbo);
      }
    }
    return result;
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES* out) override {
    return inner_->GetDeviceCapabilities(id, flags, out);
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override {
    const int percent = g_vibration.load(std::memory_order_relaxed);
    if (vibration && percent != 100) {
      X_INPUT_VIBRATION scaled = *vibration;
      auto scale = [&](uint16_t speed) {
        return uint16_t(std::min<uint32_t>(uint32_t(speed) * uint32_t(percent) / 100u, 0xFFFF));
      };
      scaled.left_motor_speed = scale(vibration->left_motor_speed);
      scaled.right_motor_speed = scale(vibration->right_motor_speed);
      return inner_->SetDeviceVibration(id, &scaled);
    }
    return inner_->SetDeviceVibration(id, vibration);
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags, X_INPUT_KEYSTROKE* out) override {
    return inner_->GetDeviceKeystroke(id, flags, out);
  }
  void OnWindowAvailable(rex::ui::Window* window) override { inner_->OnWindowAvailable(window); }

 private:
  std::unique_ptr<sdl::SDLInputDriver> inner_;
};

// Android's touch controls (GameActivity's TouchControlsView, through
// nativeTouchPad): the controls held (bit = PadControl) and the sticks.
std::atomic<uint32_t> g_touch_controls{0};
std::atomic<uint64_t> g_touch_sticks{0};  // lx, ly, rx, ry: int16 each
std::atomic<bool> g_touch_suspended{false};

// The touch controls as a pad: synthetic, so it feeds player 1 alongside any
// real one, with the same remap and Turbo.
class TouchPadDriver final : public InputDriver {
 public:
  TouchPadDriver() : InputDriver(nullptr, 0) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = kDevice;
    info.name = "Touch";
    info.synthetic = true;
    out.push_back(info);
  }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out) return X_ERROR_SUCCESS;
    const uint32_t controls = g_touch_controls.load(std::memory_order_relaxed);
    const uint64_t sticks = g_touch_sticks.load(std::memory_order_relaxed);
    std::memset(out, 0, sizeof(*out));
    const uint64_t state = uint64_t(controls) ^ (sticks * 0x9E3779B97F4A7C15ull);
    if (state != last_state_) {
      last_state_ = state;
      ++packet_;
    }
    out->packet_number = packet_;
    X_INPUT_GAMEPAD& pad = out->gamepad;
    uint16_t buttons = 0;
    for (int c = 0; c < kPadControlCount; ++c) {
      if (controls & (1u << c)) buttons |= kBits[c];
    }
    pad.buttons = buttons;
    pad.left_trigger = (controls & (1u << kPadLT)) ? 255 : 0;
    pad.right_trigger = (controls & (1u << kPadRT)) ? 255 : 0;
    pad.thumb_lx = int16_t(sticks & 0xFFFF);
    pad.thumb_ly = int16_t((sticks >> 16) & 0xFFFF);
    pad.thumb_rx = int16_t((sticks >> 32) & 0xFFFF);
    pad.thumb_ry = int16_t((sticks >> 48) & 0xFFFF);
    const PadMap map = GetPadMap();
    if (map != IdentityPadMap()) RemapPad(pad, map);
    if (const uint32_t turbo = g_turbo.load(std::memory_order_relaxed)) ApplyTurbo(pad, turbo);
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* caps) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;
      caps->sub_type = 0x01;
      caps->gamepad.buttons = 0xFFFF;
      caps->gamepad.left_trigger = 0xFF;
      caps->gamepad.right_trigger = 0xFF;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) override {
    return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  static constexpr auto kDevice = static_cast<DeviceId>(0x544F5543);  // 'TOUC'
  uint64_t last_state_ = ~0ull;
  uint32_t packet_ = 0;
};

}  // namespace

void SetTouchPad(uint32_t controls, float lx, float ly, float rx, float ry) {
  auto axis = [](float v) {
    return uint64_t(uint16_t(int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f)));
  };
  g_touch_controls.store(controls & ((1u << kPadControlCount) - 1), std::memory_order_relaxed);
  g_touch_sticks.store(axis(lx) | axis(ly) << 16 | axis(rx) << 32 | axis(ry) << 48,
                       std::memory_order_relaxed);
}
void SetTouchControlsSuspended(bool suspended) { g_touch_suspended = suspended; }
bool TouchControlsSuspended() { return g_touch_suspended; }

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
void SetVibrationStrength(int percent) { g_vibration = std::clamp(percent, 0, 200); }
void SetTurboControls(uint32_t mask) { g_turbo = mask & ((1u << kPadControlCount) - 1); }
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
#ifdef __ANDROID__
    input->AddDriver(std::make_unique<TouchPadDriver>());
#endif
    auto mnk = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk->Setup() == X_STATUS_SUCCESS) input->AddDriver(std::move(mnk));
  }
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, tool_mode ? 0 : 1));
  input->SetDeviceAssignment(std::make_unique<SlotAssignment>());
  return input;
}

}  // namespace asura::input

#ifdef __ANDROID__
// GameActivity.nativeTouchPad (TouchControlsView): the touch controls' state,
// sticks -1..1 with up positive (XInput's sense).
extern "C" JNIEXPORT void JNICALL Java_org_asurawrath_recompiled_GameActivity_nativeTouchPad(
    JNIEnv*, jclass, jint controls, jfloat lx, jfloat ly, jfloat rx, jfloat ry) {
  asura::input::SetTouchPad(uint32_t(controls), lx, ly, rx, ry);
}
// GameActivity.nativeTouchSuspended: the game's own (ImGui) menu is open and
// takes touches as the mouse, so the controls step aside.
extern "C" JNIEXPORT jboolean JNICALL Java_org_asurawrath_recompiled_GameActivity_nativeTouchSuspended(
    JNIEnv*, jclass) {
  return asura::input::TouchControlsSuspended() ? JNI_TRUE : JNI_FALSE;
}
#endif
