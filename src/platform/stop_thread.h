#pragma once
// A stand-in for std::jthread / std::stop_token: the Android NDK's libc++
// still keeps those experimental (not built without -fexperimental-library).
// Same use: the function takes a StopToken, and destroying or reassigning the
// StopThread asks it to stop and joins it.

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

namespace asura {

class StopToken {
 public:
  explicit StopToken(const std::atomic<bool>* flag) : flag_(flag) {}
  bool stop_requested() const { return flag_->load(std::memory_order_relaxed); }

 private:
  const std::atomic<bool>* flag_;
};

class StopThread {
 public:
  StopThread() = default;
  template <typename F>
  explicit StopThread(F&& f) : stop_(std::make_unique<std::atomic<bool>>(false)) {
    thread_ = std::thread([fn = std::forward<F>(f), token = StopToken(stop_.get())]() mutable {
      fn(token);
    });
  }
  StopThread(StopThread&&) noexcept = default;
  StopThread& operator=(StopThread&& other) noexcept {
    if (this != &other) {
      Stop();
      thread_ = std::move(other.thread_);
      stop_ = std::move(other.stop_);
    }
    return *this;
  }
  ~StopThread() { Stop(); }

  bool joinable() const { return thread_.joinable(); }
  void Stop() {
    if (!thread_.joinable()) return;
    stop_->store(true, std::memory_order_relaxed);
    thread_.join();
  }

 private:
  std::unique_ptr<std::atomic<bool>> stop_;
  std::thread thread_;
};

}  // namespace asura
