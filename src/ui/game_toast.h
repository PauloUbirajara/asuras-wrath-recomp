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

#include <chrono>
#include <deque>
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

class GameToastDialog : public rex::ui::AchievementNotificationDialog {
 public:
  GameToastDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
                  rex::Runtime* runtime, ImFont* font);
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

  static constexpr float kDisplaySeconds = 4.5f;
  static constexpr float kFadeIn = 0.3f, kFadeOut = 0.5f;

  Image LoadImage(const char* name);

  rex::ui::ImmediateDrawer* immediate_drawer_;
  ImFont* font_;
  rex::ui::AchievementIconCache icon_cache_;
  Image background_, portrait_;
  std::mutex mutex_;
  std::deque<Pending> queue_;
};

}  // namespace asura::ui
