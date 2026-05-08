// Copyright 2025 asyncio-cpp authors. All rights reserved.
// WaitFor and TimeoutScope — timeout wrappers and RAII cancellation guards.

#ifndef ASYNCIO_WAIT_H_
#define ASYNCIO_WAIT_H_

#include <chrono>
#include <exception>
#include <type_traits>
#include <utility>

#include "asyncio/error.h"
#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/task.h"
#include "asyncio/timer_handle.h"

namespace asyncio {

// ---------------------------------------------------------------------------
// WaitFor — timeout wrapper for Futures.
// ---------------------------------------------------------------------------

/// Wraps a Future with a timeout. If the inner Future does not complete
/// within the given duration, the wrapper is resolved with
/// AsyncTimeoutError and the inner Future is cancelled.
///
/// If the inner Future completes before the timeout, the timer is cancelled
/// and the wrapper resolves with the inner Future's result.
template <typename T>
Future<T> WaitFor(Future<T> inner, std::chrono::nanoseconds timeout) {
  Future<T> result;
  auto* loop = EventLoop::Current();

  auto timer = loop->CallLater(timeout, [result, inner]() mutable {
    if (!result.Done()) {
      // Set exception first so the wrapper resolves as timed-out before
      // inner's done callback fires (which would otherwise cancel result).
      result.SetException(
          std::make_exception_ptr(AsyncTimeoutError()));
      inner.Cancel();
    }
  });

  inner.AddDoneCallback([result, timer](Future<T>& f) mutable {
    timer.Cancel();
    if (!result.Done()) {
      if (f.Cancelled()) {
        result.Cancel();
      } else if (f.GetException()) {
        result.SetException(f.GetException());
      } else {
        if constexpr (std::is_void_v<T>) {
          result.SetResult();
        } else {
          result.SetResult(std::move(f.Result()));
        }
      }
    }
  });

  return result;
}

// ---------------------------------------------------------------------------
// TimeoutScope — RAII cancellation-scope guard.
// ---------------------------------------------------------------------------

/// RAII guard that schedules a task cancellation after a given duration.
/// On destruction, cancels the timer and calls Uncancel() if the timeout
/// fired.
///
/// Usage:
///   Task<void> MyTask() {
///     // Need a reference to the current task from the caller
///     TimeoutScope scope(5s, some_task);
///     co_await LongOperation();
///   }
template <typename T>
class TimeoutScope {
 public:
  /// Creates a timeout scope that will cancel the given task after duration.
  TimeoutScope(std::chrono::nanoseconds duration, Task<T>& task)
      : task_(&task) {
    auto* loop = EventLoop::Current();
    timer_ = loop->CallLater(duration, [this]() {
      fired_ = true;
      task_->Cancel();
    });
  }

  /// Cancels the timer. If the timeout fired, calls Uncancel() on the task.
  ~TimeoutScope() {
    timer_.Cancel();
    if (fired_) {
      task_->Uncancel();
    }
  }

  /// Non-copyable, non-movable.
  TimeoutScope(const TimeoutScope&) = delete;
  TimeoutScope& operator=(const TimeoutScope&) = delete;
  TimeoutScope(TimeoutScope&&) = delete;
  TimeoutScope& operator=(TimeoutScope&&) = delete;

  /// Returns true if the timeout fired and the task was cancelled.
  [[nodiscard]] bool Fired() const { return fired_; }

 private:
  Task<T>* task_;
  TimerHandle timer_;
  bool fired_ = false;
};

}  // namespace asyncio

#endif  // ASYNCIO_WAIT_H_
