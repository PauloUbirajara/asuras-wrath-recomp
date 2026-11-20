// Graphics effects the player can turn off (Settings > Display > Effects on
// the desktop, Settings > Graphics on Android).
//
// Each one is a [SystemSettings] switch the game reads at start from its own
// config (Coalesced_<LANG>.bin, Xbox360-BCEngine.ini). When any is off, the
// game is given a copy of the config with those switches set to False
// (ApplyGraphicsEffects), so a change applies the next time the game starts.
// launcher.cfg keeps them as fx_<key>=0/1; the Android launcher
// (LauncherActivity.kt EFFECTS) uses the same keys.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rex {
class Runtime;
}

namespace asura::game {

struct GraphicsEffect {
  const char* key;             // launcher.cfg: fx_<key>
  const char* label;           // launcher text (translated in launcher_text.cpp)
  const char* ini_keys[3];     // [SystemSettings] switches it turns off
};

inline constexpr GraphicsEffect kGraphicsEffects[] = {
    {"bloom", "Bloom", {"Bloom"}},
    {"depth_of_field", "Depth of field (background blur)", {"DepthOfField"}},
    {"motion_blur", "Motion blur", {"MotionBlur", "MotionBlurPause"}},
    {"radial_blur", "Radial blur", {"AllowRadialBlur"}},
    {"shadows", "Dynamic shadows",
     {"DynamicShadows", "LightEnvironmentShadows", "bAllowWholeSceneDominantShadows"}},
    {"ambient_occlusion", "Ambient occlusion", {"AmbientOcclusion"}},
    {"light_shafts", "Light shafts", {"bAllowLightShafts"}},
    {"lens_flares", "Lens flares", {"LensFlares"}},
    {"distortion", "Heat haze and distortion", {"Distortion", "FilteredDistortion"}},
};
inline constexpr size_t kGraphicsEffectCount = sizeof(kGraphicsEffects) / sizeof(kGraphicsEffects[0]);

// `off`: bit n = kGraphicsEffects[n] turned off. Call before the game reads its
// config (OnPreLaunchModule). Nothing to do when no bit is set.
void ApplyGraphicsEffects(rex::Runtime* runtime, uint32_t off);

}  // namespace asura::game
