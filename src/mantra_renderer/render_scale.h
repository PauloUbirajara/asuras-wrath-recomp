#pragma once
// mantra's internal resolution, settable while the game runs. mantra is a
// shared library the SDK loads at runtime, so the launcher calls this through
// the loaded library (RequestRenderScale in src/ui/launcher.cpp).

#if defined(_WIN32)
#define MANTRA_EXPORT extern "C" __declspec(dllexport)
#else
#define MANTRA_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Host pixels per guest pixel (1.0 = the game's 1280x720), clamped to
// 0.05..4. Takes effect at the next frame.
MANTRA_EXPORT void mantra_set_render_scale(double factor);
inline constexpr const char* kMantraSetRenderScale = "mantra_set_render_scale";
