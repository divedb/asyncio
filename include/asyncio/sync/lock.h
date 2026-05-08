// Copyright 2025 asyncio-cpp authors. All rights reserved.
// AsyncLock — cooperative mutex with RAII guard.

#ifndef ASYNCIO_SYNC_LOCK_H_
#define ASYNCIO_SYNC_LOCK_H_

#include <deque>

#include "asyncio/future.h"

namespace asyncio {

/// A cooperative mutex. Only one coroutine can hold the lock at a time.
/// Waiters are queued in FIFO order for fairness.
///
/// Mirrors Python's `asyncio.Lock`.
class AsyncLock {
 public:
  AsyncLock() = default;

  /// Returns true if the lock is currently held.
  [[nodiscard]] bool Locked() const { return locked_; }

  /// Acquires the lock. If it is free, acquires immediately (returns a
  /// resolved Future). Otherwise, enqueues a waiter and returns a pending
  /// Future that resolves when the lock is granted.
  Future<void> Acquire() {
    if (!locked_ && waiters_.empty()) {
      locked_ = true;
      Future<void> f;
      f.SetResult();
      return f;
    }
    waiters_.emplace_back();
    return waiters_.back();
  }

  /// Releases the lock. Wakes the first non-cancelled waiter, if any.
  /// Throws InvalidStateError if the lock is not currently held.
  void Release() {
    if (!locked_) {
      throw InvalidStateError("Lock is not acquired");
    }
    // Skip cancelled waiters.
    while (!waiters_.empty()) {
      auto waiter = std::move(waiters_.front());
      waiters_.pop_front();
      if (!waiter.Done()) {
        // Transfer ownership to this waiter.
        waiter.SetResult();
        return;
      }
    }
    // No waiters — unlock.
    locked_ = false;
  }

  /// RAII guard that releases the lock on destruction.
  class LockGuard {
   public:
    explicit LockGuard(AsyncLock& lock) : lock_(&lock) {}
    ~LockGuard() {
      if (lock_) lock_->Release();
    }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
    LockGuard(LockGuard&& other) noexcept : lock_(other.lock_) {
      other.lock_ = nullptr;
    }
    LockGuard& operator=(LockGuard&&) = delete;

   private:
    AsyncLock* lock_;
  };

 private:
  bool locked_ = false;
  std::deque<Future<void>> waiters_;
};

}  // namespace asyncio

#endif  // ASYNCIO_SYNC_LOCK_H_
