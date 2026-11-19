#pragma once
// A short line of text at the top of the screen for a couple of seconds
// ("Frame 1234 traced"), over the game. Attached to the ImGui drawer only
// while it shows (see game_toast.h for why).

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>

namespace asura::ui {

class NoticeDialog : public rex::ui::ImGuiDialog {
 public:
  NoticeDialog(rex::ui::ImGuiDrawer* drawer, ImFont* font,
               std::function<void(std::function<void()>)> call_in_ui_thread);
  ~NoticeDialog() override;

  // Thread-safe. Replaces the line showing, if any.
  void Show(std::string text);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  static constexpr float kSeconds = 2.5f, kFadeIn = 0.15f, kFadeOut = 0.4f;
  ImFont* font_;
  std::function<void(std::function<void()>)> call_in_ui_thread_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::mutex mutex_;
  std::string text_;  // under mutex_
  std::chrono::steady_clock::time_point since_;
  bool attached_ = false;  // UI thread only
};

}  // namespace asura::ui
