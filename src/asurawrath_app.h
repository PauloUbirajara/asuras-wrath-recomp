#pragma once

#include <chrono>
#include <cstdio>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include "generated/default/asura_wrath_init.h"

static PPCFunc* g_orig_sub_829B1C08 = nullptr;
static PPCFunc* g_orig_sub_829B1CA8 = nullptr;

static void HLE_RHISetStreamSource(PPCContext& ctx, uint8_t* base) {
  std::printf("[HLE] RHISetStreamSource [0x829B1C08]: Stream Index=%u, VB Guest Address=0x%08X, Offset/Stride=%u\n",
              ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  if (g_orig_sub_829B1C08) {
    g_orig_sub_829B1C08(ctx, base);
  }
}

static void HLE_RHIDrawIndexedPrimitive(PPCContext& ctx, uint8_t* base) {
  std::printf("[HLE] RHIDrawIndexedPrimitive [0x829B1CA8]: Cmd Struct=0x%08X, PrimitiveType=%u, BaseVertexIdx=%u\n",
              ctx.r7.u32, ctx.r4.u32, ctx.r5.u32);
  if (g_orig_sub_829B1CA8) {
    g_orig_sub_829B1CA8(ctx, base);
  }
}

class AsurawrathApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<AsurawrathApp>(new AsurawrathApp(ctx, "asura_wrath_recomp", PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::filesystem::create_directories("cache");
    paths.cache_root = std::filesystem::absolute("cache");

    if (paths.game_data_root.empty()) {
      if (std::filesystem::exists("extracted")) {
        paths.game_data_root = std::filesystem::absolute("extracted");
      } else if (std::filesystem::exists("game_data")) {
        paths.game_data_root = std::filesystem::absolute("game_data");
      } else {
        paths.game_data_root = std::filesystem::current_path();
      }
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }

    rex::cvar::SetFlagByName("license_mask", "4294967295");
  }

  void OnPostSetup() override {
    SetGuestFrameStats([]() -> rex::ui::FrameStats {
      static uint64_t frame_count = 0;
      static auto last_time = std::chrono::steady_clock::now();

      frame_count++;
      auto now = std::chrono::steady_clock::now();
      double delta_ms = std::chrono::duration<double, std::milli>(now - last_time).count();
      last_time = now;

      static double smoothed_fps = 60.0;
      double current_fps = (delta_ms > 0.001) ? (1000.0 / delta_ms) : 60.0;
      smoothed_fps = (smoothed_fps * 0.9) + (current_fps * 0.1);

      return rex::ui::FrameStats{
          .frame_time_ms = delta_ms,
          .fps = smoothed_fps,
          .frame_count = frame_count,
      };
    });
  }

  void OnPreLaunchModule() override {
    if (auto* rt = runtime()) {
      if (auto* dispatcher = rt->function_dispatcher()) {
        g_orig_sub_829B1C08 = sub_829B1C08;
        g_orig_sub_829B1CA8 = sub_829B1CA8;

        dispatcher->SetFunction(0x829B1C08, HLE_RHISetStreamSource);
        dispatcher->SetFunction(0x829B1CA8, HLE_RHIDrawIndexedPrimitive);

        std::printf("[HLE] Registered HLE RHI render hooks for 0x829B1C08 and 0x829B1CA8\n");
      }
    }
  }
};
