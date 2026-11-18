#pragma once
// Achievement unlock toast in Asura's Wrath's look (replaces the SDK toast via
// AsuraWrathApp::CreateAchievementNotificationDialog).
//
// Layout, left to right: [portrait] [achievement icon] [title / name (score)],
// over a background. Optional images, found by GameUiFile():
//   toast_background.png  drawn stretched over the whole toast (300x70 at
//                         720p, scaled with the window height)
//   toast_portrait.png    drawn at the left, full toast height minus padding,
//                         keeping its aspect ratio
// Without it the toast is a plain lacquer panel. Everything fades in and
// out together (text, score, images).
//
// The dialog is attached to the ImGui drawer only while a toast is queued:
// while any dialog is attached, the SDK repaints the window continuously
// (uncapped with the immediate present mode, ~1000 fps), which costs GPU
// time the game needs on integrated GPUs.

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include <imgui.h>
#include <rex/ui/overlay/achievement_icon_cache.h>
#include <rex/ui/overlay/achievement_notification.h>

namespace rex {
class Runtime;
}
namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace asura::ui {

// Settings > Achievement pop-ups (from the launcher settings).
struct ToastOptions {
  bool enabled = true;
  int corner = 3;  // 0 top left, 1 top right, 2 bottom left, 3 bottom right
  float seconds = 4.5f;
};
void SetToastOptions(const ToastOptions& options);
ToastOptions GetToastOptions();

class GameToastDialog : public rex::ui::AchievementNotificationDialog {
 public:
  GameToastDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
                  rex::Runtime* runtime, ImFont* font,
                  std::function<void(std::function<void()>)> call_in_ui_thread);
  ~GameToastDialog() override;

  // Thread-safe (guest threads unlock achievements).
  void Push(const rex::system::AchievementEvent& event) override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct Image {
    std::unique_ptr<rex::ui::ImmediateTexture> texture;
    float width = 0, height = 0;
  };
  struct Pending {
    rex::system::AchievementEvent event;
    std::optional<std::chrono::steady_clock::time_point> visible_since;
  };

  static constexpr float kFadeIn = 0.3f, kFadeOut = 0.5f;

  Image LoadImage(const char* name);

  rex::ui::ImmediateDrawer* immediate_drawer_;
  ImFont* font_;
  rex::ui::AchievementIconCache icon_cache_;
  Image background_, portrait_;
  std::function<void(std::function<void()>)> call_in_ui_thread_;
  // Cleared by the destructor, so attach calls queued before it do nothing.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::mutex mutex_;
  std::deque<Pending> queue_;
  bool attached_ = false;        // in the drawer's dialog list (UI thread)
  bool attach_pending_ = false;  // an attach call is queued
};

}  // namespace asura::ui
