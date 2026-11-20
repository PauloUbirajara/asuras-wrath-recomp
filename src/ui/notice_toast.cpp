#include "notice_toast.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "platform/platform.h"

namespace asura::ui {

namespace {
std::mutex g_trace_mutex;
TraceOptions g_trace;
}  // namespace

void SetTraceOptions(const TraceOptions& options) {
  {
    std::lock_guard lock(g_trace_mutex);
    g_trace = options;
    g_trace.position = std::clamp(options.position, 0, 5);
  }
  // Already loaded by the SDK in the game's process; only looked up.
  using SetCapture = void (*)(int);
  if (auto set = reinterpret_cast<SetCapture>(
          platform::LoadedLibrarySymbol("rexgpu-mantra", "mantra_set_trace_capture"))) {
    set(options.picture ? 1 : 0);
  }
}

TraceOptions GetTraceOptions() {
  std::lock_guard lock(g_trace_mutex);
  return g_trace;
}

NoticeDialog::NoticeDialog(rex::ui::ImGuiDrawer* drawer, ImFont* font,
                           std::function<void(std::function<void()>)> call_in_ui_thread)
    : ImGuiDialog(drawer), font_(font), call_in_ui_thread_(std::move(call_in_ui_thread)) {
  // Attached by the base constructor: stay detached until there is a line.
  imgui_drawer()->RemoveDialog(this);
}

NoticeDialog::~NoticeDialog() {
  *alive_ = false;
  if (attached_) imgui_drawer()->RemoveDialog(this);
}

void NoticeDialog::Show(std::string text) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    text_ = std::move(text);
    since_ = std::chrono::steady_clock::now();
  }
  if (!call_in_ui_thread_) return;
  call_in_ui_thread_([this, alive = alive_] {
    if (!*alive || attached_) return;
    attached_ = true;
    imgui_drawer()->AddDialog(this);
  });
}

void NoticeDialog::OnDraw(ImGuiIO& io) {
  std::string text;
  float age;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    age = std::chrono::duration<float>(std::chrono::steady_clock::now() - since_).count();
    text = text_;
  }
  if (age >= kSeconds || text.empty()) {
    if (attached_) {
      attached_ = false;
      imgui_drawer()->RemoveDialog(this);  // allowed while drawing
    }
    return;
  }
  const float alpha = std::clamp(std::min(age / kFadeIn, (kSeconds - age) / kFadeOut), 0.0f, 1.0f);
  ImFont* font = font_ ? font_ : ImGui::GetFont();
  const float size = std::max(14.0f, io.DisplaySize.y / 720.0f * 18.0f);
  const ImVec2 text_size = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
  const ImVec2 pad(size * 0.8f, size * 0.4f);
  // Settings > Frame traces > Position: a third of the width per column.
  const int position = GetTraceOptions().position;
  const float margin = size * 1.2f;
  const int column = position % 3;
  const float x = column == 0   ? margin
                  : column == 1 ? (io.DisplaySize.x - text_size.x) * 0.5f
                                : io.DisplaySize.x - text_size.x - margin;
  const float y = position < 3 ? margin : io.DisplaySize.y - text_size.y - margin;
  const ImVec2 at(std::floor(x), std::floor(y));
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  draw->AddRectFilled(ImVec2(at.x - pad.x, at.y - pad.y),
                      ImVec2(at.x + text_size.x + pad.x, at.y + text_size.y + pad.y),
                      ImGui::GetColorU32(ImVec4(0, 0, 0, 0.6f * alpha)), size * 0.3f);
  draw->AddText(font, size, at, ImGui::GetColorU32(ImVec4(1.0f, 0.93f, 0.78f, alpha)), text.c_str());
}

}  // namespace asura::ui
