// Copyright 2025 asyncio-cpp authors. All rights reserved.
// TimerHandle — a Handle scheduled at an absolute time point.

#ifndef ASYNCIO_TIMER_HANDLE_H_
#define ASYNCIO_TIMER_HANDLE_H_

#include <chrono>
#include <functional>
#include <memory>

#include "asyncio/handle.h"

namespace asyncio {

/// A Handle that is scheduled to run at a specific time.
///
/// TimerHandle extends Handle with an absolute deadline (steady_clock
/// time_point). TimerHandles are stored in a min-heap ordered by their
/// deadline, matching Python's `asyncio.TimerHandle`.
///
/// Like Handle, TimerHandle uses shared state for cancellation.
class TimerHandle : public Handle {
 public:
  /// Constructs a TimerHandle with an absolute deadline and callback.
  TimerHandle(std::chrono::steady_clock::time_point when,
              std::function<void()> callback);

  /// Default construct produces a null handle.
  TimerHandle() = default;

  /// Copyable — shares state.
  TimerHandle(const TimerHandle&) = default;
  TimerHandle& operator=(const TimerHandle&) = default;

  /// Move-constructible.
  TimerHandle(TimerHandle&& other) noexcept = default;
  TimerHandle& operator=(TimerHandle&& other) noexcept = default;

  /// Returns the absolute deadline for this timer.
  [[nodiscard]] std::chrono::steady_clock::time_point When() const;

  /// Comparison operators for heap ordering. Earlier deadlines sort first.
  [[nodiscard]] bool operator<(const TimerHandle& other) const;
  [[nodiscard]] bool operator>(const TimerHandle& other) const;

 private:
  struct TimerState {
    std::chrono::steady_clock::time_point when;
  };

  std::shared_ptr<TimerState> timer_state_;
};

}  // namespace asyncio

#endif  // ASYNCIO_TIMER_HANDLE_H_
