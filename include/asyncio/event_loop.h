// Copyright 2025 asyncio-cpp authors. All rights reserved.
// EventLoop — the central event loop for async scheduling.

#ifndef ASYNCIO_EVENT_LOOP_H_
#define ASYNCIO_EVENT_LOOP_H_

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "asyncio/handle.h"
#include "asyncio/timer_handle.h"
#include "asyncio/detail/selector.h"
#include "asyncio/detail/selector_backend.h"
#include "asyncio/detail/self_pipe.h"
#include "asyncio/detail/timer_heap.h"

namespace asyncio {

/// A single-threaded event loop that schedules and runs callbacks.
///
/// The loop maintains two work queues:
///   - A ready deque for immediate callbacks (FIFO).
///   - A timer min-heap for delayed callbacks (earliest deadline first).
///
/// I/O multiplexing is delegated to a platform-specific Selector backend
/// (kqueue on macOS, epoll on Linux, select() as fallback).
///
/// Each call to RunOnce() performs one "tick":
///   1. Clean up cancelled timers (lazy).
///   2. Compute selector timeout.
///   3. Call selector_->Select(timeout) to wait for I/O / timer expiry.
///   4. Move expired timers to the ready deque.
///   5. Run exactly N callbacks, where N is the ready deque size captured
///      before execution starts (new callbacks are deferred to the next tick).
///
/// This matches Python's `asyncio.BaseEventLoop._run_once()` semantics.
class EventLoop {
 public:
  /// Maximum select timeout, matching Python's MAXIMUM_SELECT_TIMEOUT.
  static constexpr std::chrono::seconds kMaximumTimeout{86400};

  EventLoop();

  ~EventLoop();

  /// Non-copyable, non-movable.
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop(EventLoop&&) = delete;
  EventLoop& operator=(EventLoop&&) = delete;

  // --- Lifecycle ---

  /// Runs the event loop until Stop() is called.
  void RunForever();

  /// Performs a single iteration of the event loop.
  void RunOnce();

  /// Requests the event loop to stop. Takes effect at the end of the
  /// current RunOnce() tick.
  void Stop();

  /// Returns true if the loop is currently inside RunForever().
  [[nodiscard]] bool IsRunning() const;

  // --- Callback scheduling ---

  /// Schedules a callback for immediate execution (next tick).
  /// NOT thread-safe. Must be called from the event loop thread.
  Handle CallSoon(std::function<void()> callback);

  /// Schedules a callback after the given delay.
  /// NOT thread-safe. Must be called from the event loop thread.
  TimerHandle CallLater(std::chrono::nanoseconds delay,
                        std::function<void()> callback);

  /// Schedules a callback at an absolute time point.
  /// NOT thread-safe. Must be called from the event loop thread.
  TimerHandle CallAt(std::chrono::steady_clock::time_point when,
                     std::function<void()> callback);

  /// Thread-safe version of CallSoon. Can be called from any thread.
  /// Uses the self-pipe to wake the event loop.
  Handle CallSoonThreadsafe(std::function<void()> callback);

  // --- I/O registration ---

  /// Registers fd for read-ready notifications.
  /// Calls callback whenever fd becomes readable.
  /// NOT thread-safe. Must be called from the event loop thread.
  void AddReader(int fd, std::function<void()> callback);

  /// Removes a previously registered read callback for fd.
  /// No-op if fd was not registered for reading.
  /// NOT thread-safe.
  void RemoveReader(int fd);

  /// Registers fd for write-ready notifications.
  /// Calls callback whenever fd becomes writable.
  /// NOT thread-safe.
  void AddWriter(int fd, std::function<void()> callback);

  /// Removes a previously registered write callback for fd.
  /// No-op if fd was not registered for writing.
  /// NOT thread-safe.
  void RemoveWriter(int fd);

  // --- Time ---

  /// Returns the current time from a monotonic clock.
  [[nodiscard]] std::chrono::steady_clock::time_point Time() const;

  // --- Global access ---

  /// Returns the event loop associated with the current thread.
  /// Returns nullptr if no loop is running on this thread.
  [[nodiscard]] static EventLoop* Current();

  /// Sets the event loop for the current thread.
  /// Used internally by RunForever(); exposed for Runner/Run().
  static void SetCurrent(EventLoop* loop);

 private:
  /// Drains the thread-safe queue into the ready deque.
  /// Called from RunOnce() on the event loop thread.
  void DrainThreadSafeQueue();

  /// Moves all expired timers from scheduled_ to ready_.
  void ProcessTimers();

  /// Updates the selector registration for fd based on current io_callbacks_.
  void UpdateSelectorRegistration(int fd);

  // --- State ---

  /// Immediate callback queue (FIFO).
  std::deque<Handle> ready_;

  /// Delayed callback min-heap, ordered by deadline.
  detail::TimerHeap scheduled_;

  /// Platform I/O multiplexer.
  std::unique_ptr<detail::Selector> selector_;

  /// Cross-thread wakeup mechanism (self-pipe read end is registered in
  /// the selector).
  detail::SelfPipe self_pipe_;

  /// I/O callbacks indexed by fd.
  struct IoCallbacks {
    std::function<void()> read_cb;
    std::function<void()> write_cb;
  };
  std::unordered_map<int, IoCallbacks> io_callbacks_;

  /// Thread-safe callback queue. Populated by CallSoonThreadsafe(),
  /// drained by RunOnce().
  std::mutex thread_safe_mutex_;
  std::vector<std::function<void()>> thread_safe_queue_;

  /// True while inside RunForever().
  bool running_ = false;

  /// Set by Stop(), checked at end of each RunOnce() tick.
  bool stopping_ = false;
};

}  // namespace asyncio

#endif  // ASYNCIO_EVENT_LOOP_H_
