#include "game_toast.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>

#include <rex/logging.h>
#include <rex/system/achievement_manager.h>
#include <rex/ui/immediate_drawer.h>

#include "game_font.h"

// stb_image, built into librexruntime (the SDK decodes achievement icons with it).
extern "C" unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x,
                                                int* y, int* channels_in_file,
                                                int desired_channels);
extern "C" void stbi_image_free(void* retval_from_stbi_load);

namespace asura::ui {
namespace {

// Game palette (UI_Fonts text over lacquer panels).
constexpr ImVec4 kPanel(0.17f, 0.07f, 0.04f, 0.92f);
constexpr ImVec4 kTitle(1.00f, 0.78f, 0.36f, 1.0f);  // gold
constexpr ImVec4 kText(0.95f, 0.89f, 0.76f, 1.0f);   // parchment
constexpr ImVec4 kScore(0.78f, 0.62f, 0.36f, 1.0f);  // dim gold

constexpr float kToastWidth = 300.0f, kToastHeight = 70.0f;

ImU32 Faded(ImVec4 c, float alpha) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

}  // namespace

GameToastDialog::GameToastDialog(rex::ui::ImGuiDrawer* drawer,
                                 rex::ui::ImmediateDrawer* immediate_drawer,
                                 rex::Runtime* runtime, ImFont* font)
    : AchievementNotificationDialog(drawer),
      immediate_drawer_(immediate_drawer),
      font_(font),
      icon_cache_(immediate_drawer, runtime) {
  background_ = LoadImage("toast_background.png");
  portrait_ = LoadImage("toast_portrait.png");
}

GameToastDialog::~GameToastDialog() = default;

GameToastDialog::Image GameToastDialog::LoadImage(const char* name) {
  Image image;
  const auto path = GameUiFile(name);
  std::ifstream file(path, std::ios::binary);
  if (!file) return image;
  const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
  int w = 0, h = 0, channels = 0;
  unsigned char* rgba = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
  if (!rgba) {
    REXLOG_WARN("toast: cannot decode {}", path.string());
    return image;
  }
  image.texture = immediate_drawer_->CreateTexture(uint32_t(w), uint32_t(h),
                                                   rex::ui::ImmediateTextureFilter::kLinear,
                                                   false, rgba);
  stbi_image_free(rgba);
  image.width = float(w);
  image.height = float(h);
  REXLOG_INFO("toast: loaded {} ({}x{})", path.string(), w, h);
  return image;
}

void GameToastDialog::Push(const rex::system::AchievementEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back({event, std::nullopt});
}

void GameToastDialog::OnDraw(ImGuiIO& io) {
  const auto now = std::chrono::steady_clock::now();
  rex::system::AchievementEvent event;
  float age = 0.0f;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      auto& front = queue_.front();
      if (!front.visible_since) front.visible_since = now;
      age = std::chrono::duration<float>(now - *front.visible_since).count();
      if (age < kDisplaySeconds) break;
      queue_.pop_front();
    }
    if (queue_.empty()) return;
    event = queue_.front().event;
  }
  const float alpha =
      std::clamp(std::min(age / kFadeIn, (kDisplaySeconds - age) / kFadeOut), 0.0f, 1.0f);

  // A fixed 300x70 toast at 720p, scaled with the window height; the
  // background art is stretched to fit it.
  const float scale = std::max(io.DisplaySize.y / 720.0f, 0.5f);
  const float margin = 16.0f * scale, pad = 8.0f * scale;
  const ImVec2 size(kToastWidth * scale, kToastHeight * scale);
  const ImVec2 pos(io.DisplaySize.x - size.x - margin, io.DisplaySize.y - size.y - margin);

  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##asura_toast", nullptr, flags)) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    const ImU32 tint = Faded(ImVec4(1, 1, 1, 1), alpha);
    if (background_.texture) {
      draw->AddImage(reinterpret_cast<ImTextureID>(background_.texture.get()), pos, end,
                     ImVec2(0, 0), ImVec2(1, 1), tint);
    } else {
      draw->AddRectFilled(pos, end, Faded(kPanel, alpha), 2.0f);
    }

    float x = pos.x + pad;
    const float inner_h = size.y - 2 * pad;
    if (portrait_.texture) {
      const float w = inner_h * portrait_.width / portrait_.height;
      draw->AddImage(reinterpret_cast<ImTextureID>(portrait_.texture.get()),
                     ImVec2(x, pos.y + pad), ImVec2(x + w, pos.y + pad + inner_h), ImVec2(0, 0),
                     ImVec2(1, 1), tint);
      x += w + pad;
    }
    if (rex::ui::ImmediateTexture* icon = icon_cache_.GetIcon(event.achievement)) {
      const float s = inner_h;
      const float y = pos.y + (size.y - s) * 0.5f;
      draw->AddImage(reinterpret_cast<ImTextureID>(icon), ImVec2(x, y), ImVec2(x + s, y + s),
                     ImVec2(0, 0), ImVec2(1, 1), tint);
      x += s + pad;
    }

    // Two text lines, vertically centred; the font shrinks for long names.
    const std::string& name = event.achievement.label;
    const std::string score = " (" + std::to_string(event.achievement.gamerscore) + "G)";
    const float avail = pos.x + size.x - pad - x;
    float font_size = 18.0f * scale;
    if (font_) ImGui::PushFont(font_, font_size);
    const float wide = std::max(ImGui::CalcTextSize("Achievement Unlocked").x,
                                ImGui::CalcTextSize((name + score).c_str()).x);
    if (wide > avail && avail > 0) {
      font_size = std::max(font_size * avail / wide, 11.0f * scale);
      if (font_) {
        ImGui::PopFont();
        ImGui::PushFont(font_, font_size);
      }
    }
    const float line = ImGui::GetTextLineHeight();
    float y = pos.y + (size.y - 2 * line - 4.0f) * 0.5f;
    draw->AddText(ImVec2(x, y), Faded(kTitle, alpha), "Achievement Unlocked");
    y += line + 4.0f;
    draw->AddText(ImVec2(x, y), Faded(kText, alpha), name.c_str());
    draw->AddText(ImVec2(x + ImGui::CalcTextSize(name.c_str()).x, y), Faded(kScore, alpha),
                  score.c_str());
    if (font_) ImGui::PopFont();
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

}  // namespace asura::ui
