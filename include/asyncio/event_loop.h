// Copyright 2025 asyncio-cpp authors. All rights reserved.
// EventLoop — the central event loop for async scheduling.

#ifndef ASYNCIO_EVENT_LOOP_H_
#define ASYNCIO_EVENT_LOOP_H_

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "asyncio/handle.h"
#include "asyncio/timer_handle.h"
#include "asyncio/detail/self_pipe.h"
#include "asyncio/detail/timer_heap.h"

namespace asyncio {

/// A single-threaded event loop that schedules and runs callbacks.
///
/// The loop maintains two work queues:
///   - A ready deque for immediate callbacks (FIFO).
///   - A timer min-heap for delayed callbacks (earliest deadline first).
///
/// Each call to RunOnce() performs one "tick":
///   1. Clean up cancelled timers (lazy).
///   2. Compute selector timeout.
///   3. Poll the self-pipe for cross-thread wakeups.
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

  // --- Time ---

  /// Returns the current time from a monotonic clock.
  [[nodiscard]] std::chrono::steady_clock::time_point Time() const;

  // --- Global access ---

  /// Returns the event loop associated with the current thread.
  /// Returns nullptr if no loop is running on this thread.
  [[nodiscard]] static EventLoop* Current();

 private:
  /// Drains the thread-safe queue into the ready deque.
  /// Called from RunOnce() on the event loop thread.
  void DrainThreadSafeQueue();

  /// Processes self-pipe events (reads and discards wake bytes).
  void ProcessSelfPipe();

  /// Moves all expired timers from scheduled_ to ready_.
  void ProcessTimers();

  // --- State ---

  /// Immediate callback queue (FIFO).
  std::deque<Handle> ready_;

  /// Delayed callback min-heap, ordered by deadline.
  detail::TimerHeap scheduled_;

  /// Cross-thread wakeup mechanism.
  detail::SelfPipe self_pipe_;

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
