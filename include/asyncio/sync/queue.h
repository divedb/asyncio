// Copyright 2025 asyncio-cpp authors. All rights reserved.
// AsyncQueue<T> — cooperative producer-consumer queue.

#ifndef ASYNCIO_SYNC_QUEUE_H_
#define ASYNCIO_SYNC_QUEUE_H_

#include <deque>
#include <utility>

#include "asyncio/error.h"
#include "asyncio/future.h"
#include "asyncio/task.h"

namespace asyncio {

/// A cooperative queue for producer-consumer patterns.
/// Supports bounded queues, task-done tracking, join, and shutdown.
///
/// Mirrors Python's `asyncio.Queue`.
template <typename T>
class AsyncQueue {
 public:
  /// Creates a queue. If max_size > 0, the queue is bounded and Put()
  /// blocks when full. If max_size == 0, the queue is unbounded.
  explicit AsyncQueue(size_t max_size = 0) : max_size_(max_size) {}

  // --- Non-blocking operations ---

  /// Returns the number of items in the queue.
  [[nodiscard]] size_t Size() const { return items_.size(); }

  /// Returns true if the queue has no items.
  [[nodiscard]] bool Empty() const { return items_.empty(); }

  /// Returns true if the queue is bounded and full.
  [[nodiscard]] bool Full() const {
    return max_size_ > 0 && items_.size() >= max_size_;
  }

  // --- Non-blocking operations ---

  /// Non-blocking version of Put().
  /// Throws QueueFullError if the queue is bounded and full.
  /// Throws QueueShutDownError if the queue has been shut down.
  void PutNowait(T item) {
    if (shutdown_) throw QueueShutDownError();

    // Satisfy a waiting getter if possible.
    while (!getters_.empty() && getters_.front().Done()) {
      getters_.pop_front();
    }
    if (!getters_.empty()) {
      auto getter = std::move(getters_.front());
      getters_.pop_front();
      items_.push_back(std::move(item));
      unfinished_tasks_++;
      getter.SetResult();
      return;
    }

    // Bounded and full — error.
    if (max_size_ > 0 && items_.size() >= max_size_) {
      throw QueueFullError();
    }

    // Add to queue.
    items_.push_back(std::move(item));
    unfinished_tasks_++;
    ResetJoinIfNeeded();
  }

  /// Non-blocking version of Get().
  /// Throws QueueEmptyError if the queue is empty.
  /// Throws QueueShutDownError if the queue has been shut down and empty.
  T GetNowait() {
    // Try to satisfy from waiting putters first.
    while (!putters_.empty()) {
      auto [val, putter_fut] = std::move(putters_.front());
      putters_.pop_front();
      if (!putter_fut.Done()) {
        items_.push_back(std::move(val));
        putter_fut.SetResult();
        break;
      }
    }

    if (!items_.empty()) {
      T item = std::move(items_.front());
      items_.pop_front();
      SatisfyPutter();
      return item;
    }

    if (shutdown_) throw QueueShutDownError();
    throw QueueEmptyError();
  }

  // --- Async operations ---

  /// Removes and returns an item from the queue. If the queue is empty,
  /// blocks until an item is available or the queue is shut down.
  Task<T> Get() {
    // Satisfy any waiting putters first.
    while (!putters_.empty()) {
      auto [val, putter_fut] = std::move(putters_.front());
      putters_.pop_front();
      if (!putter_fut.Done()) {
        items_.push_back(std::move(val));
        putter_fut.SetResult();
        break;
      }
    }

    if (!items_.empty()) {
      T item = std::move(items_.front());
      items_.pop_front();
      co_return item;
    }

    if (shutdown_) {
      throw QueueShutDownError();
    }

    // Wait for an item.
    Future<void> getter;
    getters_.push_back(getter);
    try {
      co_await getter;
    } catch (...) {
      // On cancellation, check if an item was assigned to us.
      if (!items_.empty()) {
        T item = std::move(items_.front());
        items_.pop_front();
        // Satisfy a putter if needed.
        SatisfyPutter();
        co_return item;
      }
      throw;
    }

    // After waking, item should be available.
    if (items_.empty()) {
      if (shutdown_) throw QueueShutDownError();
      // Shouldn't happen under normal use.
      throw AsyncError("Queue Get() woke but no item available");
    }

    T item = std::move(items_.front());
    items_.pop_front();
    SatisfyPutter();
    co_return item;
  }

  /// Adds an item to the queue. If the queue is bounded and full,
  /// blocks until space is available.
  Task<void> Put(T item) {
    if (shutdown_) throw QueueShutDownError();

    // Clean up cancelled getters from the front.
    while (!getters_.empty() && getters_.front().Done()) {
      getters_.pop_front();
    }

    // Satisfy a waiting getter if possible.
    if (!getters_.empty()) {
      auto getter = std::move(getters_.front());
      getters_.pop_front();
      items_.push_back(std::move(item));
      getter.SetResult();
      unfinished_tasks_++;
      co_return;
    }

    // Unbounded or not full — add directly.
    if (max_size_ == 0 || items_.size() < max_size_) {
      items_.push_back(std::move(item));
      unfinished_tasks_++;
      // If join was previously completed, reset join state.
      ResetJoinIfNeeded();
      co_return;
    }

    // Bounded and full — wait for space.
    putters_.emplace_back(std::move(item), Future<void>());
    try {
      co_await putters_.back().second;
    } catch (...) {
      // On cancellation, remove our putter entry if still pending.
      auto& [val, fut] = putters_.back();
      if (!fut.Done()) {
        putters_.pop_back();
      }
      throw;
    }
  }

  // --- Task-done / Join ---

  /// Indicate that a formerly enqueued task is complete. Used by consumers.
  /// Each call to Get() that returns an item should be matched by exactly
  /// one call to TaskDone().
  void TaskDone() {
    if (unfinished_tasks_ <= 0) {
      throw InvalidStateError("TaskDone() called too many times");
    }
    unfinished_tasks_--;
    if (unfinished_tasks_ == 0) {
      for (auto& w : join_waiters_) {
        if (!w.Done()) w.SetResult();
      }
      join_waiters_.clear();
    }
  }

  /// Blocks until all items in the queue have been gotten and processed
  /// (marked done via TaskDone()).
  Future<void> Join() {
    if (unfinished_tasks_ == 0) {
      Future<void> f;
      f.SetResult();
      return f;
    }
    join_waiters_.emplace_back();
    return join_waiters_.back();
  }

  // --- Shutdown ---

  /// Shuts down the queue. Subsequent Put() calls throw QueueShutDownError.
  /// Get() continues to return remaining items; once empty, Get() also
  /// throws QueueShutDownError.
  void Shutdown() {
    shutdown_ = true;
    // Wake all waiting getters with an error.
    auto getters = std::move(getters_);
    getters_.clear();
    for (auto& g : getters) {
      if (!g.Done()) {
        g.SetException(std::make_exception_ptr(QueueShutDownError()));
      }
    }
    // Wake all waiting putters with an error.
    auto putters = std::move(putters_);
    putters_.clear();
    for (auto& [val, fut] : putters) {
      if (!fut.Done()) {
        fut.SetException(std::make_exception_ptr(QueueShutDownError()));
      }
    }
  }

 private:
  std::deque<T> items_;
  std::deque<Future<void>> getters_;
  std::deque<std::pair<T, Future<void>>> putters_;
  int unfinished_tasks_ = 0;
  std::deque<Future<void>> join_waiters_;
  bool shutdown_ = false;
  size_t max_size_;

  /// Satisfy one waiting putter if the queue has space.
  void SatisfyPutter() {
    while (!putters_.empty()) {
      auto& [val, fut] = putters_.front();
      if (!fut.Done()) {
        items_.push_back(std::move(val));
        fut.SetResult();
        putters_.pop_front();
        return;
      }
      putters_.pop_front();
    }
  }

  /// Reset join state when new items are added after a previous join cycle.
  void ResetJoinIfNeeded() {
    // join_waiters_ are only non-empty while unfinished_tasks_ > 0.
    // If tasks just completed (join waiters resolved) and new items arrive,
    // we just let the new cycle work naturally.
  }
};

}  // namespace asyncio

#endif  // ASYNCIO_SYNC_QUEUE_H_
