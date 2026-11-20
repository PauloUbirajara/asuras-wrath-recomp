#include "controller_setup.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include "../input/controller_mappings.h"
#include "launcher_text.h"

namespace asura::ui {
namespace {

enum class Kind { kButton, kTrigger, kStick };

struct Step {
  const char* field;   // SDL mapping output ("-lefty": the left stick's up half)
  const char* label;   // the row (translated, launcher_text.cpp)
  const char* prompt;  // while listening (translated)
  Kind kind;
};

constexpr Step kStepList[] = {
    {"a", "A", "Press A (the bottom face button)", Kind::kButton},
    {"b", "B", "Press B (the right face button)", Kind::kButton},
    {"x", "X", "Press X (the left face button)", Kind::kButton},
    {"y", "Y", "Press Y (the top face button)", Kind::kButton},
    {"leftshoulder", "Left shoulder", "Press the left bumper (LB / L1)", Kind::kButton},
    {"rightshoulder", "Right shoulder", "Press the right bumper (RB / R1)", Kind::kButton},
    {"lefttrigger", "Left trigger", "Pull the left trigger (LT / L2)", Kind::kTrigger},
    {"righttrigger", "Right trigger", "Pull the right trigger (RT / R2)", Kind::kTrigger},
    {"back", "Back", "Press Back (Select, View, Share)", Kind::kButton},
    {"start", "Start", "Press Start (Menu, Options)", Kind::kButton},
    {"leftstick", "Left stick press", "Click the left stick in (L3)", Kind::kButton},
    {"rightstick", "Right stick press", "Click the right stick in (R3)", Kind::kButton},
    {"dpup", "D-pad up", "Press up on the D-pad", Kind::kButton},
    {"dpdown", "D-pad down", "Press down on the D-pad", Kind::kButton},
    {"dpleft", "D-pad left", "Press left on the D-pad", Kind::kButton},
    {"dpright", "D-pad right", "Press right on the D-pad", Kind::kButton},
    {"-lefty", "Left stick up", "Push the left stick up", Kind::kStick},
    {"+lefty", "Left stick down", "Push the left stick down", Kind::kStick},
    {"-leftx", "Left stick left", "Push the left stick left", Kind::kStick},
    {"+leftx", "Left stick right", "Push the left stick right", Kind::kStick},
    {"-righty", "Right stick up", "Push the right stick up", Kind::kStick},
    {"+righty", "Right stick down", "Push the right stick down", Kind::kStick},
    {"-rightx", "Right stick left", "Push the right stick left", Kind::kStick},
    {"+rightx", "Right stick right", "Push the right stick right", Kind::kStick},
};
static_assert(std::size(kStepList) == ControllerSetup::kSteps);

constexpr int kPressed = 16384;  // an axis this far from rest counts
constexpr int kLetGo = 8000;     // and back within this, let go

int StepOf(std::string_view field) {
  for (int s = 0; s < ControllerSetup::kSteps; ++s) {
    if (field == kStepList[s].field) return s;
  }
  return -1;
}

// "b3" -> "Button 3", "h0.1" -> "Hat 0 up", "+a2" -> "Axis 2 +", "a5" -> "Axis 5".
std::string Describe(const std::string& input) {
  if (input.empty()) return T("Not set");
  char text[64];
  if (input[0] == 'b') {
    std::snprintf(text, sizeof(text), T("Button %s"), input.c_str() + 1);
  } else if (input[0] == 'h') {
    const int mask = std::atoi(input.c_str() + input.find('.') + 1);
    const char* dir = mask == SDL_HAT_UP ? "up" : mask == SDL_HAT_DOWN ? "down"
                    : mask == SDL_HAT_LEFT ? "left" : "right";
    std::snprintf(text, sizeof(text), "%s %s %s", T("Hat"),
                  input.substr(1, input.find('.') - 1).c_str(), T(dir));
  } else {
    const bool half = input[0] == '+' || input[0] == '-';
    std::snprintf(text, sizeof(text), "%s %s%s%s", T("Axis"),
                  input.substr(half ? 2 : 1).c_str(), half ? " " : "",
                  half ? std::string(1, input[0]).c_str() : "");
  }
  return text;
}

std::string GuidOf(SDL_JoystickID id) {
  char text[64] = {};
  SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), text, sizeof(text));
  return text;
}

}  // namespace

ControllerSetup::ControllerSetup() {
  subsystem_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
}

ControllerSetup::~ControllerSetup() {
  Close();
  if (subsystem_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void ControllerSetup::Open(SDL_JoystickID id) {
  if (joystick_ && SDL_GetJoystickID(joystick_) == id) return;
  Close();
  joystick_ = SDL_OpenJoystick(id);
  chosen_ = id;
  guid_ = GuidOf(id);
  const char* name = SDL_GetJoystickNameForID(id);
  name_ = name ? name : "Controller";
  std::replace(name_.begin(), name_.end(), ',', ' ');
  RecordRest();
  Load();
}

void ControllerSetup::Close() {
  if (joystick_) SDL_CloseJoystick(joystick_);
  joystick_ = nullptr;
  listening_ = -1;
  sequence_ = false;
}

void ControllerSetup::RecordRest() {
  if (!joystick_) return;
  rest_.assign(std::max(0, SDL_GetNumJoystickAxes(joystick_)), 0);
  for (size_t a = 0; a < rest_.size(); ++a) rest_[a] = SDL_GetJoystickAxis(joystick_, int(a));
}

// The rows from the mapping SDL uses now (the saved setup, its database's,
// or none): whole stick axes split into their halves.
void ControllerSetup::Load() {
  inputs_.fill({});
  extra_.clear();
  saved_ = !input::SavedControllerMapping(guid_).empty();
  char* mapping = SDL_GetGamepadMappingForID(chosen_);
  if (!mapping) return;
  std::string_view rest(mapping);
  for (int skip = 0; skip < 2 && !rest.empty(); ++skip) {  // GUID, name
    const size_t comma = rest.find(',');
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
  }
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    const std::string_view field = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    const size_t colon = field.find(':');
    if (field.empty() || colon == std::string_view::npos) continue;
    const std::string key(field.substr(0, colon));
    const std::string value(field.substr(colon + 1));
    if (const int s = StepOf(key); s >= 0) {
      inputs_[s] = value;
      continue;
    }
    const bool stick = key == "leftx" || key == "lefty" || key == "rightx" || key == "righty";
    if (stick && value.size() >= 2 && value[0] == 'a') {
      const bool inverted = value.back() == '~';
      const std::string axis = value.substr(1, value.size() - 1 - (inverted ? 1 : 0));
      inputs_[StepOf("-" + key)] = (inverted ? "+a" : "-a") + axis;
      inputs_[StepOf("+" + key)] = (inverted ? "-a" : "+a") + axis;
      continue;
    }
    extra_ += std::string(field) + ",";
  }
  SDL_free(mapping);
}

void ControllerSetup::Listen(int step, bool sequence) {
  listening_ = step;
  sequence_ = sequence;
  listen_since_ = SDL_GetTicks();
  armed_ = false;
  pending_button_.clear();
  message_.clear();
}

bool ControllerSetup::Released() const {
  for (int b = 0; b < SDL_GetNumJoystickButtons(joystick_); ++b) {
    if (SDL_GetJoystickButton(joystick_, b)) return false;
  }
  for (int h = 0; h < SDL_GetNumJoystickHats(joystick_); ++h) {
    if (SDL_GetJoystickHat(joystick_, h) != SDL_HAT_CENTERED) return false;
  }
  for (size_t a = 0; a < rest_.size(); ++a) {
    if (std::abs(SDL_GetJoystickAxis(joystick_, int(a)) - rest_[a]) > kLetGo) return false;
  }
  return true;
}

// One answer, saved at once. An input another control had is taken from it.
void ControllerSetup::Set(int step, const std::string& input) {
  for (auto& other : inputs_) {
    if (other == input) other.clear();
  }
  inputs_[step] = input;
  if (input::SaveControllerMapping(guid_, Mapping())) {
    saved_ = true;
  } else {
    message_ = T("Couldn't save the setup.");
  }
}

void ControllerSetup::Capture() {
  const uint64_t now = SDL_GetTicks();
  if (now - listen_since_ > uint64_t(kListenSeconds) * 1000) {
    listening_ = -1;
    sequence_ = false;
    message_ = T("Nothing was pressed in 10 seconds: stopped.");
    return;
  }
  if (!armed_) {
    if (Released()) {
      armed_ = true;
    } else if (now - listen_since_ > 1500) {
      // An axis SDL hadn't read at the start (some report 0 until moved):
      // what the controller reads now, with no button held, is its rest.
      bool buttons = false;
      for (int b = 0; b < SDL_GetNumJoystickButtons(joystick_); ++b) {
        buttons |= SDL_GetJoystickButton(joystick_, b);
      }
      if (!buttons) {
        RecordRest();
        armed_ = true;
      }
    }
    return;
  }
  const Kind kind = kStepList[listening_].kind;
  std::string button, axis;
  for (int b = 0; b < SDL_GetNumJoystickButtons(joystick_) && button.empty(); ++b) {
    if (SDL_GetJoystickButton(joystick_, b)) button = "b" + std::to_string(b);
  }
  for (int h = 0; h < SDL_GetNumJoystickHats(joystick_) && button.empty(); ++h) {
    const Uint8 v = SDL_GetJoystickHat(joystick_, h);
    if (v == SDL_HAT_UP || v == SDL_HAT_DOWN || v == SDL_HAT_LEFT || v == SDL_HAT_RIGHT) {
      button = "h" + std::to_string(h) + "." + std::to_string(v);
    }
  }
  int best = kPressed;
  for (size_t a = 0; a < rest_.size(); ++a) {
    const int v = SDL_GetJoystickAxis(joystick_, int(a));
    const int moved = std::abs(v - rest_[a]);
    if (moved <= best) continue;
    best = moved;
    const std::string n = std::to_string(a);
    if (kind == Kind::kTrigger && rest_[a] < -kPressed) {
      axis = "a" + n;  // a trigger resting at one end: the whole axis
    } else if (kind == Kind::kTrigger && rest_[a] > kPressed) {
      axis = "a" + n + "~";
    } else {
      axis = (v > rest_[a] ? "+a" : "-a") + n;
    }
  }
  std::string got;
  if (kind == Kind::kButton) {
    got = !button.empty() ? button : axis;
  } else if (kind == Kind::kStick) {
    got = !axis.empty() ? axis : button;
  } else if (!axis.empty()) {
    got = axis;
  } else if (!button.empty()) {
    // A trigger may also send a button: wait a moment for its axis.
    if (pending_button_.empty()) {
      pending_button_ = button;
      pending_since_ = now;
    } else if (now - pending_since_ > 200) {
      got = pending_button_;
    }
  }
  if (got.empty()) return;
  const int step = listening_;
  Set(step, got);
  if (sequence_ && step + 1 < kSteps) {
    Listen(step + 1, true);
  } else {
    if (sequence_) message_ = T("Done: every control is set.");
    listening_ = -1;
    sequence_ = false;
  }
}

std::string ControllerSetup::Mapping() const {
  std::string out = guid_ + "," + name_ + ",";
  for (int s = 0; s < kSteps; ++s) {
    if (kStepList[s].kind == Kind::kStick || inputs_[s].empty()) continue;
    out += std::string(kStepList[s].field) + ":" + inputs_[s] + ",";
  }
  // A stick axis: whole when its two halves are one axis's, else by halves.
  for (const std::string stick : {"leftx", "lefty", "rightx", "righty"}) {
    const std::string& neg = inputs_[StepOf("-" + stick)];
    const std::string& pos = inputs_[StepOf("+" + stick)];
    if (neg.size() > 2 && pos.size() > 2 && neg.substr(1) == pos.substr(1) && neg[1] == 'a') {
      if (neg[0] == '-' && pos[0] == '+') {
        out += stick + ":a" + neg.substr(2) + ",";
        continue;
      }
      if (neg[0] == '+' && pos[0] == '-') {
        out += stick + ":a" + neg.substr(2) + "~,";
        continue;
      }
    }
    if (!neg.empty()) out += "-" + stick + ":" + neg + ",";
    if (!pos.empty()) out += "+" + stick + ":" + pos + ",";
  }
  return out + extra_;
}

bool ControllerSetup::Back() {
  if (listening_ < 0) return false;
  listening_ = -1;
  sequence_ = false;
  return true;
}

void ControllerSetup::Draw(float scale) {
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(T("For a controller the game reads wrongly (a trigger moving the camera, a "
                           "stick doing nothing). Click Set next to a control and press it on the "
                           "controller, or Start setup to be asked for each in turn. Each waits 10 "
                           "seconds, and is saved at once."));
  ImGui::Spacing();
  if (!subsystem_) {
    ImGui::TextDisabled("%s", SDL_GetError());
    ImGui::PopTextWrapPos();
    return;
  }
  SDL_UpdateJoysticks();
  if (joystick_ && !SDL_JoystickConnected(joystick_)) {
    Close();
    message_ = T("The controller was disconnected.");
  }

  int count = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&count);
  if (count == 0) {
    SDL_free(ids);
    Close();
    ImGui::TextDisabled("%s", T("No controller found. Connect one to set it up."));
    if (!message_.empty()) ImGui::TextUnformatted(message_.c_str());
    ImGui::PopTextWrapPos();
    return;
  }
  bool found = false;
  for (int i = 0; i < count; ++i) found |= ids[i] == chosen_;
  if (!found) chosen_ = ids[0];
  if (!joystick_ || SDL_GetJoystickID(joystick_) != chosen_) Open(chosen_);

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(T("Controller"));
  ImGui::SameLine(160 * scale);
  ImGui::SetNextItemWidth(360 * scale);
  ImGui::BeginDisabled(Listening());
  const char* current = SDL_GetJoystickNameForID(chosen_);
  if (ImGui::BeginCombo("##setup_pad", current ? current : "?")) {
    for (int i = 0; i < count; ++i) {
      const char* name = SDL_GetJoystickNameForID(ids[i]);
      ImGui::PushID(int(ids[i]));
      if (ImGui::Selectable(name ? name : "?", ids[i] == chosen_)) Open(ids[i]);
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  SDL_free(ids);
  ImGui::TextDisabled("%s", saved_ ? T("This controller uses your setup.")
                                   : T("This controller uses SDL's own mapping."));
  ImGui::Spacing();

  if (Listening() && joystick_) Capture();

  if (Listening()) {
    const int left = kListenSeconds - int((SDL_GetTicks() - listen_since_) / 1000);
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.5f);
    ImGui::Text("%s  (%d s)", T(kStepList[listening_].prompt), std::max(left, 0));
    ImGui::PopFont();
    if (!armed_) ImGui::TextDisabled("%s", T("Let go of everything first."));
    if (sequence_) {
      if (ImGui::Button(T("Skip"))) {
        if (listening_ + 1 < kSteps) {
          Listen(listening_ + 1, true);
        } else {
          Back();
        }
      }
      ImGui::SameLine();
    }
    if (ImGui::Button(T("Stop"))) Back();
  } else {
    if (ImGui::Button(T("Start setup"))) Listen(0, true);
    if (saved_) {
      ImGui::SameLine();
      if (ImGui::Button(T("Forget my setup"))) {
        input::RemoveControllerMapping(guid_);
        saved_ = false;
        message_ = T("Forgotten: SDL's own mapping applies from the next start.");
      }
    }
  }
  if (!message_.empty()) ImGui::TextUnformatted(message_.c_str());
  ImGui::Spacing();

  if (ImGui::BeginTable("##setup_rows", 3, ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##input", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##set", ImGuiTableColumnFlags_WidthFixed);
    for (int s = 0; s < kSteps; ++s) {
      ImGui::PushID(s);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(T(kStepList[s].label));
      ImGui::TableNextColumn();
      if (s == listening_) {
        ImGui::TextUnformatted("...");
      } else if (inputs_[s].empty()) {
        ImGui::TextDisabled("%s", Describe(inputs_[s]).c_str());
      } else {
        ImGui::TextUnformatted(Describe(inputs_[s]).c_str());
      }
      ImGui::TableNextColumn();
      if (ImGui::SmallButton(T("Set"))) Listen(s, false);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::PopTextWrapPos();
}

}  // namespace asura::ui
