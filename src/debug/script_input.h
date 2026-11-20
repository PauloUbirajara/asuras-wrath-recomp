#pragma once
// Debug: a scripted gamepad, so test runs can reach menus and scenes without a
// person at the controls.
//
//   ASURA_INPUT_SCRIPT="12:A,15:START,20.5:DOWN,21:A,90:TRACE"
//
// Each entry is <seconds since the driver was added>:<action>. A button action
// presses it for 0.15 s: A B X Y START BACK UP DOWN LEFT RIGHT LB RB LT RT
// LS RS. TRACE touches $MANTRA_TRACE_TRIGGER (the renderer traces the next
// frame). The device is synthetic, so it drives player 1 alongside any real pad.

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <rex/input/input_driver.h>
#include <rex/logging.h>

namespace asura::debug {

using namespace rex;         // X_STATUS, X_RESULT, X_ERROR_*
using namespace rex::input;  // X_INPUT_*

class ScriptInputDriver : public rex::input::InputDriver {
 public:
  // nullptr if ASURA_INPUT_SCRIPT is unset or empty.
  static std::unique_ptr<ScriptInputDriver> FromEnvironment() {
    const char* script = std::getenv("ASURA_INPUT_SCRIPT");
    if (!script || !*script) return nullptr;
    auto driver = std::unique_ptr<ScriptInputDriver>(new ScriptInputDriver());
    std::string text(script);
    size_t at = 0;
    while (at <= text.size()) {
      const size_t end = std::min(text.find(',', at), text.size());
      const std::string item = text.substr(at, end - at);
      at = end + 1;
      const size_t colon = item.find(':');
      if (colon == std::string::npos) continue;
      Event event;
      event.time = std::strtod(item.c_str(), nullptr);
      event.action = item.substr(colon + 1);
      event.buttons = Buttons(event.action);
      event.trigger = event.action == "LT" ? 1 : event.action == "RT" ? 2 : 0;
      // RSLEFT / RSRIGHT: right stick fully left / right (camera turns).
      event.stick = event.action == "RSLEFT" ? -32767 : event.action == "RSRIGHT" ? 32767 : 0;
      if (!event.buttons && !event.trigger && !event.stick && event.action != "TRACE") {
        REXLOG_WARN("ASURA_INPUT_SCRIPT: unknown action '{}'", event.action);
        continue;
      }
      driver->events_.push_back(event);
    }
    REXLOG_INFO("ASURA_INPUT_SCRIPT: {} events", driver->events_.size());
    return driver;
  }

  X_STATUS Setup() override {
    start_ = std::chrono::steady_clock::now();
    return X_STATUS_SUCCESS;
  }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kDevice;
    info.name = "Script";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t rx = 0;
    for (Event& e : events_) {
      if (now < e.time) continue;
      if (e.action == "TRACE") {
        if (!e.done) {
          e.done = true;
          if (const char* trigger = std::getenv("MANTRA_TRACE_TRIGGER"); trigger && *trigger) {
            std::ofstream(trigger).put('\n');
            REXLOG_INFO("ASURA_INPUT_SCRIPT: trace at {:.1f} s", now);
          }
        }
        continue;
      }
      if (now < e.time + kPressSeconds) {
        buttons |= e.buttons;
        if (e.trigger == 1) lt = 0xFF;
        if (e.trigger == 2) rt = 0xFF;
        if (e.stick) rx = int16_t(e.stick);
      }
    }
    if (out) {
      std::memset(out, 0, sizeof(*out));
      // A new packet number whenever the state changes.
      const uint64_t state =
          buttons | uint64_t(lt) << 16 | uint64_t(rt) << 24 | uint64_t(uint16_t(rx)) << 32;
      if (state != last_state_) {
        last_state_ = state;
        ++packet_;
      }
      out->packet_number = packet_;
      out->gamepad.buttons = buttons;
      out->gamepad.left_trigger = lt;
      out->gamepad.right_trigger = rt;
      out->gamepad.thumb_rx = rx;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                 X_INPUT_CAPABILITIES* caps) override {
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

  X_RESULT SetDeviceVibration(rex::input::DeviceId id, X_INPUT_VIBRATION*) override {
    return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) override {
    return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  struct Event {
    double time = 0;
    std::string action;
    uint16_t buttons = 0;
    int trigger = 0;
    int stick = 0;
    bool done = false;
  };
  static constexpr double kPressSeconds = 0.15;
  static constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x53435249);  // 'SCRI'

  ScriptInputDriver() : InputDriver(nullptr, 0) {}

  static uint16_t Buttons(const std::string& a) {
    static const struct {
      const char* name;
      uint16_t bits;
    } kNames[] = {{"A", X_INPUT_GAMEPAD_A},
                  {"B", X_INPUT_GAMEPAD_B},
                  {"X", X_INPUT_GAMEPAD_X},
                  {"Y", X_INPUT_GAMEPAD_Y},
                  {"START", X_INPUT_GAMEPAD_START},
                  {"BACK", X_INPUT_GAMEPAD_BACK},
                  {"UP", X_INPUT_GAMEPAD_DPAD_UP},
                  {"DOWN", X_INPUT_GAMEPAD_DPAD_DOWN},
                  {"LEFT", X_INPUT_GAMEPAD_DPAD_LEFT},
                  {"RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT},
                  {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER},
                  {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
                  {"LS", X_INPUT_GAMEPAD_LEFT_THUMB},
                  {"RS", X_INPUT_GAMEPAD_RIGHT_THUMB}};
    for (const auto& n : kNames) {
      if (a == n.name) return n.bits;
    }
    return 0;
  }

  std::vector<Event> events_;
  std::chrono::steady_clock::time_point start_;
  uint64_t last_state_ = 0;
  uint32_t packet_ = 0;
};

}  // namespace asura::debug
