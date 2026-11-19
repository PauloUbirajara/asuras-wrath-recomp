#pragma once
// Logging for the mantra renderer, as the "gpu.mantra" subcategory of the
// SDK's gpu category, so it follows the gpu level and per-category overrides.
// The *_NOISY macros are for per-frame messages and only print with
// --log_noisy.
#include <string>

#include <fmt/format.h>
#include <rex/logging.h>

REXLOG_DEFINE_SUBCATEGORY(mantra, gpu)

#define MANTRA_TRACE(...) REXLOG_CAT_TRACE(::rex::log::gpu_mantra(), __VA_ARGS__)
#define MANTRA_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::gpu_mantra(), __VA_ARGS__)
#define MANTRA_INFO(...) REXLOG_CAT_INFO(::rex::log::gpu_mantra(), __VA_ARGS__)
#define MANTRA_WARN(...) REXLOG_CAT_WARN(::rex::log::gpu_mantra(), __VA_ARGS__)
#define MANTRA_ERROR(...) REXLOG_CAT_ERROR(::rex::log::gpu_mantra(), __VA_ARGS__)
#define MANTRA_NOISY_DEBUG(...) REXLOG_CAT_NOISY_DEBUG(::rex::log::gpu_mantra(), __VA_ARGS__)
#define MANTRA_NOISY_TRACE(...) REXLOG_CAT_NOISY_TRACE(::rex::log::gpu_mantra(), __VA_ARGS__)

namespace mantra {
// A frame trace (F3, MANTRA_TRACE_*) writes its lines to a file of its own in
// $MANTRA_TRACE_DIR (Files folder/traces) while the traced frame runs; without
// that folder, or outside that frame, they go to the log as "trace: ...".
void TraceLine(std::string line);
}  // namespace mantra
#define MANTRA_FRAME_TRACE(...) ::mantra::TraceLine(fmt::format(__VA_ARGS__))

// Logs only the first time a given call site is reached.
#define MANTRA_WARN_ONCE(...)                       \
  do {                                              \
    static std::atomic<bool> mantra_warned_{false}; \
    if (!mantra_warned_.exchange(true)) {           \
      MANTRA_WARN(__VA_ARGS__);                     \
    }                                               \
  } while (false)
