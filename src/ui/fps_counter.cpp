#include "fps_counter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cstdio>
#include <mutex>

#include <dlfcn.h>

namespace asura::ui {

namespace {

using SwapCount = uint64_t (*)();

SwapCount SwapCounter() {
  static const SwapCount counter = [] () -> SwapCount {
    // Already loaded by the SDK (RTLD_LOCAL); RTLD_NOLOAD only finds it.
    void* lib = dlopen("librexgpu-mantra.so", RTLD_NOW | RTLD_NOLOAD);
    return lib ? reinterpret_cast<SwapCount>(dlsym(lib, "mantra_guest_swap_count")) : nullptr;
  }();
  return counter;
}

std::atomic<int> g_corner{0};

}  // namespace

void SetFpsCorner(int corner) { g_corner = std::clamp(corner, 0, 3); }

rex::ui::FrameStats GuestFrameStats() {
  using Clock = std::chrono::steady_clock;
  static std::mutex mutex;
  static Clock::time_point window_start = Clock::now();
  static uint64_t window_swaps = 0;
  static rex::ui::FrameStats stats;
  const SwapCount counter = SwapCounter();
  if (!counter) return {};
  std::lock_guard lock(mutex);
  const uint64_t swaps = counter();
  const Clock::time_point now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - window_start).count();
  if (seconds >= 0.5) {
    const uint64_t frames = swaps - window_swaps;
    stats.fps = double(frames) / seconds;
    stats.frame_time_ms = frames ? seconds * 1000.0 / double(frames) : 0.0;
    window_start = now;
    window_swaps = swaps;
  }
  stats.frame_count = swaps;
  return stats;
}

FpsCounterDialog::FpsCounterDialog(rex::ui::ImGuiDrawer* drawer, ImFont* font)
    : ImGuiDialog(drawer), font_(font) {}

void FpsCounterDialog::OnDraw(ImGuiIO& io) {
  const rex::ui::FrameStats stats = GuestFrameStats();
  char text[32];
  std::snprintf(text, sizeof(text), "%.0f FPS", stats.fps);
  ImFont* font = font_ ? font_ : ImGui::GetFont();
  const float size = std::max(14.0f, io.DisplaySize.y / 720.0f * 18.0f);
  const float margin = size * 0.6f;
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  const ImVec2 text_size = font->CalcTextSizeA(size, FLT_MAX, 0, text);
  const int corner = g_corner.load(std::memory_order_relaxed);
  const ImVec2 at(corner & 1 ? io.DisplaySize.x - margin - text_size.x : margin,
                  corner & 2 ? io.DisplaySize.y - margin - text_size.y : margin);
  draw->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), text);
  draw->AddText(font, size, at, IM_COL32(255, 236, 200, 255), text);
}

}  // namespace asura::ui
