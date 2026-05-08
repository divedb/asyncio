// Copyright 2025 asyncio-cpp authors. All rights reserved.

#include "asyncio/event_loop.h"

#include <algorithm>
#include <utility>

#include "asyncio/policy.h"

namespace asyncio {

namespace {
thread_local EventLoop* current_loop = nullptr;
}  // namespace

EventLoop::EventLoop()
    : selector_(std::make_unique<detail::DefaultSelector>()) {
  // Register the self-pipe read end so that CallSoonThreadsafe() can
  // unblock a blocking Select() call from another thread.
  selector_->Register(self_pipe_.ReadFd(), detail::kReadable);
}

EventLoop::~EventLoop() {
  // Unregister self-pipe before it's destroyed.
  selector_->Unregister(self_pipe_.ReadFd());
}

// --- Lifecycle ---

void EventLoop::RunForever() {
  auto* prev = current_loop;
  current_loop = this;
  running_ = true;
  stopping_ = false;

  // Note: The running loop is set by Run() or Runner::Run() before
  // RunForever() is called. We only set it here if it wasn't set yet.
  bool we_set_running_loop = (GetRunningLoop() != this);
  if (we_set_running_loop) {
    EventLoopPolicy::SetRunningLoop(this);
  }

  while (!stopping_) {
    RunOnce();
  }
  running_ = false;
  stopping_ = false;
  current_loop = prev;

  // Clear the running loop marker only if we set it.
  if (we_set_running_loop) {
    EventLoopPolicy::SetRunningLoop(nullptr);
  }
}

void EventLoop::RunOnce() {
  // 1. Clean up cancelled timers (lazy, amortized).
  scheduled_.MaybeRebuild();

  // 2. Compute how long the selector should wait for I/O events.
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
  } else {
    // No ready callbacks, no timers — poll only (don't block forever).
    select_timeout = std::chrono::nanoseconds::zero();
  }

  // 3. Wait for I/O readiness (or timeout).
  auto io_events = selector_->Select(select_timeout);

  // 4. Dispatch I/O events.
  for (const auto& ev : io_events) {
    // Handle self-pipe wakeup.
    if (ev.fd == self_pipe_.ReadFd()) {
      self_pipe_.Drain();
      DrainThreadSafeQueue();
      continue;
    }

    auto it = io_callbacks_.find(ev.fd);
    if (it == io_callbacks_.end()) continue;

    if (ev.readable() && it->second.read_cb) {
      // Schedule callback rather than calling directly, so that I/O
      // callbacks have the same deferred semantics as CallSoon callbacks.
      ready_.emplace_back(it->second.read_cb);
    }
    if (ev.writable() && it->second.write_cb) {
      ready_.emplace_back(it->second.write_cb);
    }
  }

  // 5. Drain any thread-safe callbacks that arrived without a self-pipe
  //    wakeup (e.g., before selector blocked).
  DrainThreadSafeQueue();

  // 6. Move expired timers from scheduled_ to ready_.
  ProcessTimers();

  // 7. Run exactly ntodo callbacks, where ntodo is captured before
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

// --- I/O registration ---

void EventLoop::AddReader(int fd, std::function<void()> callback) {
  auto& cbs = io_callbacks_[fd];
  cbs.read_cb = std::move(callback);
  UpdateSelectorRegistration(fd);
}

void EventLoop::RemoveReader(int fd) {
  auto it = io_callbacks_.find(fd);
  if (it == io_callbacks_.end()) return;
  it->second.read_cb = nullptr;
  if (!it->second.write_cb) {
    selector_->Unregister(fd);
    io_callbacks_.erase(it);
  } else {
    UpdateSelectorRegistration(fd);
  }
}

void EventLoop::AddWriter(int fd, std::function<void()> callback) {
  auto& cbs = io_callbacks_[fd];
  cbs.write_cb = std::move(callback);
  UpdateSelectorRegistration(fd);
}

void EventLoop::RemoveWriter(int fd) {
  auto it = io_callbacks_.find(fd);
  if (it == io_callbacks_.end()) return;
  it->second.write_cb = nullptr;
  if (!it->second.read_cb) {
    selector_->Unregister(fd);
    io_callbacks_.erase(it);
  } else {
    UpdateSelectorRegistration(fd);
  }
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

void EventLoop::UpdateSelectorRegistration(int fd) {
  auto it = io_callbacks_.find(fd);
  if (it == io_callbacks_.end()) return;

  uint32_t events = 0;
  if (it->second.read_cb) events |= detail::kReadable;
  if (it->second.write_cb) events |= detail::kWritable;

  // Check whether this fd is already in the selector's registered map
  // by attempting a Modify first; fall back to Register if not found.
  // We track this via a simple sentinel: newly added fds are Registered,
  // existing ones are Modified.
  //
  // A cleaner solution would be to expose IsRegistered(), but to keep
  // the Selector interface minimal we use a try/catch approach here.
  try {
    selector_->Modify(fd, events);
  } catch (const std::invalid_argument&) {
    // Not registered yet — do a fresh Register.
    selector_->Register(fd, events);
  }
}

// --- Global access ---

EventLoop* EventLoop::Current() { return current_loop; }

void EventLoop::SetCurrent(EventLoop* loop) { current_loop = loop; }

}  // namespace asyncio
