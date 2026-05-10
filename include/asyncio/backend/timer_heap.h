// Copyright 2025 asyncio-cpp authors. All rights reserved.
// TimerHeap — min-heap of TimerHandles with lazy cancellation cleanup.

#ifndef ASYNCIO_DETAIL_TIMER_HEAP_H_
#define ASYNCIO_DETAIL_TIMER_HEAP_H_

#include <chrono>
#include <vector>

#include "asyncio/timer_handle.h"

namespace asyncio::detail {

/// A min-heap of TimerHandles ordered by deadline.
///
/// Provides O(log n) insertion and extraction. Cancelled handles are not
/// removed immediately; instead, they are lazily cleaned up when the heap
/// becomes polluted (more than 50% cancelled and size > 100), matching
/// Python's `_run_once()` timer cleanup strategy.
class TimerHeap {
 public:
  /// Pushes a TimerHandle onto the heap. O(log n).
  void Push(TimerHandle handle);

  /// Removes and returns the top (earliest-deadline) TimerHandle.
  /// Skips over cancelled handles. O(log n) amortized.
  /// Precondition: !Empty().
  TimerHandle Pop();

  /// Returns a const reference to the top TimerHandle (earliest deadline).
  /// May return a cancelled handle — caller must check.
  /// Precondition: !Empty().
  [[nodiscard]] const TimerHandle& Top() const;

  /// Returns true if the heap contains no handles (including cancelled ones).
  [[nodiscard]] bool Empty() const;

  /// Returns the number of handles (including cancelled ones).
  [[nodiscard]] int Size() const;

  /// Notifies the heap that an entry was cancelled from the outside.
  /// Used by the event loop to track cancellation pollution.
  void NotifyCancelled();

  /// Performs lazy cleanup: if the heap is large and more than 50% of
  /// entries are cancelled, rebuilds the heap from scratch. Otherwise a no-op.
  void MaybeRebuild();

  /// Clears all handles from the heap.
  void Clear();

 private:
  /// Removes all cancelled handles and restores the heap invariant.
  void Rebuild();

  /// Heap operations (index-based).
  void SiftUp(int index);
  void SiftDown(int index);
  static int Parent(int index) { return (index - 1) / 2; }
  static int LeftChild(int index) { return 2 * index + 1; }
  static int RightChild(int index) { return 2 * index + 2; }

  std::vector<TimerHandle> heap_;
  int cancelled_count_ = 0;
};

}  // namespace asyncio::detail

#endif  // ASYNCIO_DETAIL_TIMER_HEAP_H_
