// Copyright 2025 asyncio-cpp authors. All rights reserved.
// AsyncEvent — cooperative event primitive (set / clear / wait).

#ifndef ASYNCIO_SYNC_EVENT_H_
#define ASYNCIO_SYNC_EVENT_H_

#include <deque>

#include "asyncio/future.h"

namespace asyncio {

/// A cooperative event primitive. Waiters block until Set() is called.
/// All waiters are released simultaneously when the event is set.
///
/// Mirrors Python's `asyncio.Event`.
class AsyncEvent {
 public:
  AsyncEvent() = default;

  /// Returns true if the event is set.
  [[nodiscard]] bool IsSet() const { return set_; }

  /// Sets the event. All current waiters are resolved immediately.
  void Set() {
    if (set_) return;
    set_ = true;
    auto waiters = std::move(waiters_);
    waiters_.clear();
    for (auto& w : waiters) {
      if (!w.Done()) w.SetResult();
    }
  }

  /// Clears the event. Future calls to Wait() will block.
  void Clear() { set_ = false; }

  /// Returns a Future that resolves when the event is set.
  /// If already set, returns an immediately-resolved Future.
  Future<void> Wait() {
    if (set_) {
      Future<void> f;
      f.SetResult();
      return f;
    }
    waiters_.emplace_back();
    return waiters_.back();
  }

 private:
  bool set_ = false;
  std::deque<Future<void>> waiters_;
};

}  // namespace asyncio

#endif  // ASYNCIO_SYNC_EVENT_H_
