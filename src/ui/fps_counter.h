#pragma once
// The game's frame rate: guest swaps counted by mantra (mantra_guest_swap_count),
// shown by the SDK's F3 overlay (ReXApp::SetGuestFrameStats) and, when
// Graphics > "Show the frame rate" is on, in a corner of the screen.

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/debug_overlay.h>

namespace asura::ui {

// Frames per second over the last half second; zero with another GPU plugin.
rex::ui::FrameStats GuestFrameStats();

class FpsCounterDialog : public rex::ui::ImGuiDialog {
 public:
  FpsCounterDialog(rex::ui::ImGuiDrawer* drawer, ImFont* font);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  ImFont* font_;
};

}  // namespace asura::ui
