#include "frame_rate.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

namespace asura::game {
namespace {

constexpr uint32_t kGEngine = 0x83295F20;            // UEngine* GEngine
constexpr uint32_t kGetMaxTickRateSlot = 308;        // vtable byte offset

std::atomic<int> g_cap{30};
std::atomic<PPCFunc*> g_original{nullptr};
std::thread g_installer;
std::atomic<bool> g_stop{false};

// FLOAT GetMaxTickRate(FLOAT DeltaTime, ...): the rate the loop waits for, 0
// = don't wait. The original smooths the frame rate between the config's
// Min/MaxSmoothedFrameRate; with another cap the chosen rate is returned
// whenever the original asks for a wait at all.
void GetMaxTickRateHook(PPCContext& ctx, uint8_t* base) {
  if (PPCFunc* original = g_original.load()) original(ctx, base);
  const int cap = g_cap.load(std::memory_order_relaxed);
  if (cap != 30 && ctx.f1.f64 > 0.0) ctx.f1.f64 = cap > 0 ? double(cap) : 0.0;
}

uint32_t LoadGuest32(rex::memory::Memory* memory, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, memory->TranslateVirtual(address), sizeof(value));
  return rex::byte_swap(value);
}

}  // namespace

void SetFrameRateCap(int fps) {
  const int cap = fps <= 0 ? 0 : fps;
  if (g_cap.exchange(cap) != cap) {
    REXLOG_INFO("Frame rate limit: {}", cap ? std::to_string(cap) + " fps" : "unlimited");
  }
}

void StartFrameRateHook(rex::Runtime* runtime) {
  if (!runtime || g_installer.joinable()) return;
  auto* memory = runtime->memory();
  auto* dispatcher = runtime->function_dispatcher();
  if (!memory || !dispatcher) return;
  g_stop = false;
  g_installer = std::thread([memory, dispatcher] {
    // GEngine is created during the engine's start (a few seconds in).
    while (!g_stop) {
      const uint32_t engine = LoadGuest32(memory, kGEngine);
      const uint32_t vtable = engine ? LoadGuest32(memory, engine) : 0;
      const uint32_t function = vtable ? LoadGuest32(memory, vtable + kGetMaxTickRateSlot) : 0;
      if (function) {
        PPCFunc* original = dispatcher->GetFunction(function);
        if (!original) {
          REXLOG_WARN("Frame rate limit: GetMaxTickRate at {:08X} isn't recompiled code", function);
          return;
        }
        g_original = original;
        dispatcher->SetFunction(function, &GetMaxTickRateHook);
        REXLOG_INFO("Frame rate limit: hooked GetMaxTickRate at {:08X} (GEngine {:08X})", function,
                    engine);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
}

void StopFrameRateHook() {
  g_stop = true;
  if (g_installer.joinable()) g_installer.join();
}

}  // namespace asura::game
