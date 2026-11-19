#include "ui/window_icon.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>

#include "ui/game_font.h"

extern "C" unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x,
                                                int* y, int* channels_in_file, int desired_channels);
extern "C" void stbi_image_free(void* retval_from_stbi_load);

namespace asura::ui {
namespace {

// Before SDL starts its video subsystem: the app id names the .desktop file
// (asura_wrath_recomp.desktop, as in the AppImage), which Wayland desktops
// take the icon from.
const bool kHints = [] {
  SDL_SetHint(SDL_HINT_APP_ID, "asura_wrath_recomp");
  SDL_SetHint(SDL_HINT_APP_NAME, "Asura's Wrath Recompiled");
  return true;
}();

constexpr int kSize = 256;  // the art is 640 px; X11 keeps the icon as a window property

// Box-filtered downscale of RGBA pixels.
std::vector<unsigned char> Shrink(const unsigned char* rgba, int w, int h, int size) {
  std::vector<unsigned char> out(size_t(size) * size * 4);
  for (int y = 0; y < size; ++y) {
    const int y0 = y * h / size, y1 = std::max(y0 + 1, (y + 1) * h / size);
    for (int x = 0; x < size; ++x) {
      const int x0 = x * w / size, x1 = std::max(x0 + 1, (x + 1) * w / size);
      unsigned sum[4] = {};
      for (int sy = y0; sy < y1; ++sy) {
        for (int sx = x0; sx < x1; ++sx) {
          for (int c = 0; c < 4; ++c) sum[c] += rgba[(size_t(sy) * w + sx) * 4 + c];
        }
      }
      const unsigned n = unsigned((y1 - y0) * (x1 - x0));
      for (int c = 0; c < 4; ++c) out[(size_t(y) * size + x) * 4 + c] = (unsigned char)(sum[c] / n);
    }
  }
  return out;
}

}  // namespace

void ApplyWindowIcon() {
  (void)kHints;
  std::ifstream file(GameUiFile("icon.png"), std::ios::binary);
  if (!file) return;
  const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
  int w = 0, h = 0, channels = 0;
  unsigned char* rgba = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
  if (!rgba) return;
  std::vector<unsigned char> pixels = w > kSize || h > kSize
                                          ? Shrink(rgba, w, h, kSize)
                                          : std::vector<unsigned char>(rgba, rgba + size_t(w) * h * 4);
  if (w > kSize || h > kSize) w = h = kSize;
  stbi_image_free(rgba);
  SDL_Surface* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels.data(), w * 4);
  if (!surface) return;
  int count = 0;
  if (SDL_Window** windows = SDL_GetWindows(&count)) {
    for (int i = 0; i < count; ++i) SDL_SetWindowIcon(windows[i], surface);
    SDL_free(windows);
  }
  SDL_DestroySurface(surface);
}

}  // namespace asura::ui
