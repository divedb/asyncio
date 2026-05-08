// Copyright 2025 asyncio-cpp authors. All rights reserved.
// AsyncBarrier — cooperative barrier primitive.

#ifndef ASYNCIO_SYNC_BARRIER_H_
#define ASYNCIO_SYNC_BARRIER_H_

#include <deque>

#include "asyncio/error.h"
#include "asyncio/future.h"

namespace asyncio {

/// A cooperative barrier. When the configured number of parties have called
/// Wait(), all waiters are released simultaneously.
///
/// The barrier automatically resets for the next round after all parties
/// have passed.
///
/// Mirrors Python's `asyncio.Barrier` (Python 3.11+).
class AsyncBarrier {
 public:
  /// Creates a barrier for the given number of parties.
  explicit AsyncBarrier(int parties) : parties_(parties) {}

  /// Returns the number of parties required to pass the barrier.
  [[nodiscard]] int Parties() const { return parties_; }

  /// Returns the number of parties currently waiting.
  [[nodiscard]] int NWaiting() const { return count_; }

  /// Waits for all parties to arrive. The last party to call Wait()
  /// triggers the release of all waiters.
  ///
  /// Throws BrokenBarrierError if the barrier has been aborted.
  Future<void> Wait() {
    if (broken_) throw BrokenBarrierError();

    count_++;
    if (count_ == parties_) {
      // Last party — resolve all waiters.
      auto waiters = std::move(waiters_);
      waiters_.clear();
      count_ = 0;
      for (auto& w : waiters) {
        if (!w.Done()) w.SetResult();
      }
      Future<void> f;
      f.SetResult();
      return f;
    }

    // Not the last — wait.
    waiters_.emplace_back();
    return waiters_.back();
  }

  /// Breaks the barrier permanently. All current and future Wait() calls
  /// will throw BrokenBarrierError.
  void Abort() {
    if (broken_) return;
    broken_ = true;
    auto waiters = std::move(waiters_);
    waiters_.clear();
    count_ = 0;
    for (auto& w : waiters) {
      if (!w.Done()) {
        w.SetException(std::make_exception_ptr(BrokenBarrierError()));
      }
    }
  }

  /// Resets the barrier to its initial state. All waiters must have
  /// passed or the barrier must not be broken. Throws BrokenBarrierError
  /// if there are outstanding waiters (use Abort() first in that case).
  void Reset() {
    if (count_ > 0) {
      // There are threads waiting — abort them first.
      Abort();
    }
    broken_ = false;
    count_ = 0;
    waiters_.clear();
  }

 private:
  int parties_;
  int count_ = 0;
  std::deque<Future<void>> waiters_;
  bool broken_ = false;
};

}  // namespace asyncio

#endif  // ASYNCIO_SYNC_BARRIER_H_
