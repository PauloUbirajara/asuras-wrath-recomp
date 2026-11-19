#include "notice_toast.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace asura::ui {

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
  const ImVec2 at(std::floor((io.DisplaySize.x - text_size.x) * 0.5f), size * 1.2f);
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  draw->AddRectFilled(ImVec2(at.x - pad.x, at.y - pad.y),
                      ImVec2(at.x + text_size.x + pad.x, at.y + text_size.y + pad.y),
                      ImGui::GetColorU32(ImVec4(0, 0, 0, 0.6f * alpha)), size * 0.3f);
  draw->AddText(font, size, at, ImGui::GetColorU32(ImVec4(1.0f, 0.93f, 0.78f, alpha)), text.c_str());
}

}  // namespace asura::ui
