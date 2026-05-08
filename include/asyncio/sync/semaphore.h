// Copyright 2025 asyncio-cpp authors. All rights reserved.
// AsyncSemaphore — cooperative counting semaphore.

#ifndef ASYNCIO_SYNC_SEMAPHORE_H_
#define ASYNCIO_SYNC_SEMAPHORE_H_

#include <deque>

#include "asyncio/future.h"

namespace asyncio {

/// A cooperative counting semaphore. Maintains an internal counter;
/// Acquire() decrements it (blocks if zero), Release() increments it.
///
/// Mirrors Python's `asyncio.Semaphore`.
class AsyncSemaphore {
 public:
  /// Creates a semaphore with the given initial value.
  explicit AsyncSemaphore(int initial = 0) : value_(initial) {}

  /// Returns the current value.
  [[nodiscard]] int Value() const { return value_; }

  /// Acquires the semaphore. If value > 0, decrements and returns
  /// immediately. Otherwise, enqueues a waiter.
  Future<void> Acquire() {
    if (value_ > 0 && waiters_.empty()) {
      value_--;
      Future<void> f;
      f.SetResult();
      return f;
    }
    waiters_.emplace_back();
    return waiters_.back();
  }

  /// Releases the semaphore. Increments the counter and wakes the first
  /// non-cancelled waiter, if any.
  void Release() {
    while (!waiters_.empty()) {
      auto waiter = std::move(waiters_.front());
      waiters_.pop_front();
      if (!waiter.Done()) {
        // Don't increment — hand the slot directly to the waiter.
        waiter.SetResult();
        return;
      }
    }
    value_++;
  }

 private:
  int value_;
  std::deque<Future<void>> waiters_;
};

/// A bounded semaphore. Like AsyncSemaphore, but Release() throws
/// ValueError if called more times than Acquire().
///
/// Mirrors Python's `asyncio.BoundedSemaphore`.
class AsyncBoundedSemaphore : public AsyncSemaphore {
 public:
  /// Creates a bounded semaphore with the given initial value.
  explicit AsyncBoundedSemaphore(int initial = 1)
      : AsyncSemaphore(initial), initial_(initial) {}

  /// Releases the semaphore. Throws InvalidStateError if the value would
  /// exceed the initial bound.
  void Release() {
    if (Value() >= initial_) {
      throw InvalidStateError(
          "BoundedSemaphore released more than acquired");
    }
    AsyncSemaphore::Release();
  }

 private:
  int initial_;
};

}  // namespace asyncio

#endif  // ASYNCIO_SYNC_SEMAPHORE_H_
