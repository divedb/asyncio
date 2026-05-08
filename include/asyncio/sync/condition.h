// Copyright 2025 asyncio-cpp authors. All rights reserved.
// AsyncCondition — cooperative condition variable.

#ifndef ASYNCIO_SYNC_CONDITION_H_
#define ASYNCIO_SYNC_CONDITION_H_

#include <deque>

#include "asyncio/future.h"
#include "asyncio/sync/lock.h"
#include "asyncio/task.h"

namespace asyncio {

/// A cooperative condition variable. Associated with an AsyncLock.
/// Wait() atomically releases the lock, waits for notification, then
/// re-acquires the lock (even on cancellation).
///
/// Mirrors Python's `asyncio.Condition`.
class AsyncCondition {
 public:
  /// Creates a condition variable associated with the given lock.
  explicit AsyncCondition(AsyncLock& lock) : lock_(lock) {}

  /// Acquires the underlying lock (proxy to AsyncLock::Acquire()).
  Future<void> Acquire() { return lock_.Acquire(); }

  /// Releases the underlying lock (proxy to AsyncLock::Release()).
  void Release() { lock_.Release(); }

  /// Returns true if the underlying lock is held.
  [[nodiscard]] bool Locked() const { return lock_.Locked(); }

  /// Atomically releases the associated lock, waits for notification,
  /// then re-acquires the lock. The lock is always re-acquired, even if
  /// the coroutine is cancelled while waiting.
  Task<void> Wait() {
    lock_.Release();
    Future<void> waiter;
    waiters_.push_back(waiter);
    std::exception_ptr ep;
    try {
      co_await waiter;
    } catch (...) {
      ep = std::current_exception();
    }
    // Always re-acquire the lock, even on cancellation.
    // (co_await cannot appear inside a catch handler.)
    co_await lock_.Acquire();
    if (ep) {
      std::rethrow_exception(ep);
    }
  }

  /// Notifies up to `n` waiting coroutines.
  void Notify(int n = 1) {
    int count = 0;
    for (auto it = waiters_.begin();
         it != waiters_.end() && count < n;) {
      if (!it->Done()) {
        it->SetResult();
        count++;
      }
      ++it;
    }
  }

  /// Notifies all waiting coroutines.
  void NotifyAll() {
    auto waiters = std::move(waiters_);
    waiters_.clear();
    for (auto& w : waiters) {
      if (!w.Done()) w.SetResult();
    }
  }

 private:
  AsyncLock& lock_;
  std::deque<Future<void>> waiters_;
};

}  // namespace asyncio

#endif  // ASYNCIO_SYNC_CONDITION_H_
