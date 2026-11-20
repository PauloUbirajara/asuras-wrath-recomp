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

// The dialog previews go to (the latest created).
std::mutex g_preview_mutex;
GameToastDialog* g_preview_dialog = nullptr;

ImU32 Faded(ImVec4 c, float alpha) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

}  // namespace

GameToastDialog::GameToastDialog(rex::ui::ImGuiDrawer* drawer,
                                 rex::ui::ImmediateDrawer* immediate_drawer,
                                 rex::Runtime* runtime, ImFont* font,
                                 std::function<void(std::function<void()>)> call_in_ui_thread)
    : AchievementNotificationDialog(drawer),
      immediate_drawer_(immediate_drawer),
      font_(font),
      icon_cache_(immediate_drawer, runtime),
      call_in_ui_thread_(std::move(call_in_ui_thread)) {
  background_ = LoadImage("toast_background.png");
  portrait_ = LoadImage("toast_portrait.png");
  // The base constructor attached the dialog (created in the UI thread);
  // stay detached until there is something to show.
  imgui_drawer()->RemoveDialog(this);
  std::lock_guard lock(g_preview_mutex);
  g_preview_dialog = this;
}

GameToastDialog::~GameToastDialog() {
  *alive_ = false;
  std::lock_guard lock(g_preview_mutex);
  if (g_preview_dialog == this) g_preview_dialog = nullptr;
}

GameToastDialog::Image GameToastDialog::LoadImage(const char* name) {
  return LoadImageFile(GameUiFile(name));
}

GameToastDialog::Image GameToastDialog::LoadImageFile(const std::filesystem::path& path) {
  Image image;
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

namespace {
std::mutex g_options_mutex;
ToastOptions g_options;
}  // namespace

bool PreviewToast(const rex::system::AchievementEvent& sample) {
  std::lock_guard lock(g_preview_mutex);
  if (!g_preview_dialog) return false;
  g_preview_dialog->PushPreview(sample);
  return true;
}

void SetToastOptions(const ToastOptions& options) {
  std::lock_guard lock(g_options_mutex);
  g_options = options;
}

ToastOptions GetToastOptions() {
  std::lock_guard lock(g_options_mutex);
  return g_options;
}

void GameToastDialog::Push(const rex::system::AchievementEvent& event) {
  // The unlock itself is recorded by the runtime; only the pop-up is skipped.
  if (!GetToastOptions().enabled) return;
  Queue(event, false);
}

void GameToastDialog::PushPreview(const rex::system::AchievementEvent& event) {
  Queue(event, true);
}

void GameToastDialog::Queue(const rex::system::AchievementEvent& event, bool preview) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // A new preview replaces one still showing (settings change between clicks).
    if (preview && !queue_.empty() && queue_.back().preview) queue_.pop_back();
    queue_.push_back({event, std::nullopt, preview});
    if (attached_ || attach_pending_ || !call_in_ui_thread_) return;
    attach_pending_ = true;
  }
  // Outside the lock: in the UI thread this runs immediately.
  call_in_ui_thread_([this, alive = alive_] {
    if (!*alive) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      attach_pending_ = false;
      if (attached_ || queue_.empty()) return;
      attached_ = true;
    }
    imgui_drawer()->AddDialog(this);
  });
}

void GameToastDialog::OnDraw(ImGuiIO& io) {
  const ToastOptions options = GetToastOptions();
  const float display_seconds = options.seconds;
  const auto now = std::chrono::steady_clock::now();
  rex::system::AchievementEvent event;
  float age = 0.0f;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      auto& front = queue_.front();
      if (!front.visible_since) front.visible_since = now;
      age = std::chrono::duration<float>(now - *front.visible_since).count();
      if (age < display_seconds) break;
      queue_.pop_front();
    }
    if (queue_.empty()) {
      // Nothing left to show: detach (allowed while drawing) so the window
      // goes back to presenting only the game's frames.
      if (attached_) {
        attached_ = false;
        imgui_drawer()->RemoveDialog(this);
      }
      return;
    }
    event = queue_.front().event;
  }
  const float alpha =
      std::clamp(std::min(age / kFadeIn, (display_seconds - age) / kFadeOut), 0.0f, 1.0f);

  // A fixed 300x70 toast at 720p, scaled with the window height; the
  // background art is stretched to fit it.
  const float margin = 16.0f * std::max(io.DisplaySize.y / 720.0f, 0.5f);
  float scale = std::max(io.DisplaySize.y / 720.0f, 0.5f) * std::clamp(options.size, 0.5f, 2.5f);
  // Never wider than the screen (large sizes on narrow windows).
  if (kToastWidth * scale > io.DisplaySize.x - 2 * margin) {
    scale = std::max(io.DisplaySize.x - 2 * margin, 1.0f) / kToastWidth;
  }
  const float pad = 8.0f * scale;
  const ImVec2 size(kToastWidth * scale, kToastHeight * scale);
  // Column: 0 left, 1 centre, 2 right; row: 0 top, 1 middle, 2 bottom.
  static constexpr int kColumn[] = {0, 1, 2, 0, 2, 0, 1, 2};
  static constexpr int kRow[] = {0, 0, 0, 1, 1, 2, 2, 2};
  const int position = std::clamp(options.position, 0, 7);
  auto place = [&](int slot, float extent, float avail) {
    return slot == 0 ? margin : slot == 1 ? (avail - extent) * 0.5f : avail - extent - margin;
  };
  const ImVec2 pos(place(kColumn[position], size.x, io.DisplaySize.x),
                   place(kRow[position], size.y, io.DisplaySize.y));

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
    rex::ui::ImmediateTexture* icon = icon_cache_.GetIcon(event.achievement);
    if (!icon && !event.achievement.icon_path.empty() &&
        std::filesystem::path(event.achievement.icon_path).is_absolute()) {
      auto it = file_icons_.find(event.achievement.icon_path);
      if (it == file_icons_.end()) {
        it = file_icons_.emplace(event.achievement.icon_path,
                                 LoadImageFile(event.achievement.icon_path)).first;
      }
      icon = it->second.texture.get();
    }
    if (icon) {
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
    float y = pos.y + (size.y - 2 * line - 4.0f * scale) * 0.5f;
    draw->AddText(ImVec2(x, y), Faded(kTitle, alpha), "Achievement Unlocked");
    y += line + 4.0f * scale;
    draw->AddText(ImVec2(x, y), Faded(kText, alpha), name.c_str());
    draw->AddText(ImVec2(x + ImGui::CalcTextSize(name.c_str()).x, y), Faded(kScore, alpha),
                  score.c_str());
    if (font_) ImGui::PopFont();
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

}  // namespace asura::ui
