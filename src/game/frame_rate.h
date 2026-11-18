// Frame-rate cap (Graphics > Enhancements > Frame rate limit).
//
// The engine loop (sub_829125B8, UE3's FEngineLoop::Tick) asks
// GEngine->GetMaxTickRate() each frame (vtable offset 308) and sleeps off the
// rest of 1 / that rate. The game's config caps it at 30 (GameEngine
// MaxSmoothedFrameRate=30). Once GEngine exists, its GetMaxTickRate is
// replaced in the function table by a wrapper that returns the chosen cap.
#pragma once

namespace rex {
class Runtime;
}

namespace asura::game {

// 30 = the game's own limit, 0 = unlimited. Takes effect on the next frame.
void SetFrameRateCap(int fps);

// Waits (on a thread of its own) for the engine, then installs the wrapper.
void StartFrameRateHook(rex::Runtime* runtime);
void StopFrameRateHook();

}  // namespace asura::game
