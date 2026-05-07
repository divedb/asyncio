// Copyright 2025 asyncio-cpp authors. All rights reserved.

#include "asyncio/event_loop.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace asyncio {

namespace {
thread_local EventLoop* current_loop = nullptr;
}  // namespace

EventLoop::EventLoop() = default;

EventLoop::~EventLoop() = default;

// --- Lifecycle ---

void EventLoop::RunForever() {
  auto* prev = current_loop;
  current_loop = this;
  running_ = true;
  stopping_ = false;
  while (!stopping_) {
    RunOnce();
  }
  running_ = false;
  stopping_ = false;
  current_loop = prev;
}

void EventLoop::RunOnce() {
  // 1. Clean up cancelled timers (lazy, amortized).
  scheduled_.MaybeRebuild();

  // 2. Compute how long to wait for I/O.
  //    In Milestone 1 we only have the self-pipe, so we compute a timeout
  //    based on whether there is ready work and/or pending timers.
  std::optional<std::chrono::nanoseconds> select_timeout;
  if (!ready_.empty()) {
    // Ready work exists — poll only, don't block.
    select_timeout = std::chrono::nanoseconds::zero();
  } else if (!scheduled_.Empty()) {
    auto now = Time();
    auto deadline = scheduled_.Top().When();
    if (deadline <= now) {
      select_timeout = std::chrono::nanoseconds::zero();
    } else {
      auto remaining = deadline - now;
      auto capped = std::min(
          remaining,
          std::chrono::duration_cast<std::chrono::nanoseconds>(kMaximumTimeout));
      select_timeout =
          std::chrono::duration_cast<std::chrono::nanoseconds>(capped);
    }
  }
  // else: select_timeout remains nullopt — block indefinitely.

  // 3. Poll self-pipe for cross-thread wakeups.
  //    In a full implementation this would be selector_->Select(timeout).
  //    For Milestone 1 we do a targeted wait.
  if (select_timeout.has_value() && *select_timeout == std::chrono::nanoseconds::zero()) {
    // Non-blocking: just drain any pending bytes.
    ProcessSelfPipe();
  } else if (select_timeout.has_value()) {
    // Wait for the computed timeout, then check the self-pipe.
    std::this_thread::sleep_for(*select_timeout);
    ProcessSelfPipe();
  } else {
    // No work at all — block until woken by another thread.
    // In a real implementation, selector_->Select() would block here.
    // For Milestone 1, we sleep briefly and check.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ProcessSelfPipe();
  }

  // 4. Drain thread-safe queue into ready_.
  DrainThreadSafeQueue();

  // 5. Move expired timers from scheduled_ to ready_.
  ProcessTimers();

  // 6. Run exactly ntodo callbacks, where ntodo is captured before
  //    execution starts. Callbacks scheduled during execution are deferred
  //    to the next tick.
  int ntodo = static_cast<int>(ready_.size());
  for (int i = 0; i < ntodo; ++i) {
    Handle handle = std::move(ready_.front());
    ready_.pop_front();
    try {
      handle.Run();
    } catch (...) {
      // Log and continue — unhandled exceptions in callbacks must not kill
      // the event loop. This matches Python's behavior.
    }
  }
}

void EventLoop::Stop() { stopping_ = true; }

bool EventLoop::IsRunning() const { return running_; }

// --- Callback scheduling ---

Handle EventLoop::CallSoon(std::function<void()> callback) {
  Handle handle(std::move(callback));
  ready_.push_back(handle);  // Copy — shared state.
  return handle;
}

TimerHandle EventLoop::CallLater(std::chrono::nanoseconds delay,
                                  std::function<void()> callback) {
  auto when = Time() + delay;
  return CallAt(when, std::move(callback));
}

TimerHandle EventLoop::CallAt(std::chrono::steady_clock::time_point when,
                               std::function<void()> callback) {
  TimerHandle handle(when, std::move(callback));
  scheduled_.Push(handle);  // Copy — shared state.
  return handle;
}

Handle EventLoop::CallSoonThreadsafe(std::function<void()> callback) {
  {
    std::lock_guard<std::mutex> lock(thread_safe_mutex_);
    thread_safe_queue_.push_back(std::move(callback));
  }
  self_pipe_.Wakeup();
  // Return a null handle — the callback is already queued and cannot
  // be individually cancelled through this return value.
  return Handle();
}

// --- Time ---

std::chrono::steady_clock::time_point EventLoop::Time() const {
  return std::chrono::steady_clock::now();
}

// --- Private ---

void EventLoop::DrainThreadSafeQueue() {
  std::vector<std::function<void()>> stolen;
  {
    std::lock_guard<std::mutex> lock(thread_safe_mutex_);
    stolen.swap(thread_safe_queue_);
  }
  for (auto& cb : stolen) {
    ready_.emplace_back(std::move(cb));
  }
}

void EventLoop::ProcessSelfPipe() { self_pipe_.Drain(); }

void EventLoop::ProcessTimers() {
  auto now = Time();
  while (!scheduled_.Empty()) {
    // Peek at the earliest deadline.
    if (scheduled_.Top().When() > now) break;
    TimerHandle handle = scheduled_.Pop();
    if (!handle.Cancelled()) {
      ready_.emplace_back(std::move(handle));
    }
  }
}

// --- Global access ---

EventLoop* EventLoop::Current() { return current_loop; }

}  // namespace asyncio
