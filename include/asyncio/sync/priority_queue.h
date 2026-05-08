// Copyright 2025 asyncio-cpp authors. All rights reserved.
// priority_queue.h — Priority queue for asyncio.

#ifndef ASYNCIO_SYNC_PRIORITY_QUEUE_H_
#define ASYNCIO_SYNC_PRIORITY_QUEUE_H_

#include <functional>
#include <queue>

#include "asyncio/sync/queue.h"

namespace asyncio {

/// A priority queue for producer-consumer patterns.
/// Lower priority value = higher priority (like std::priority_queue).
///
/// Mirrors Python's `asyncio.PriorityQueue`.
template <typename T>
class AsyncPriorityQueue {
 public:
  /// Creates a priority queue.
  /// If max_size > 0, the queue is bounded. Default is unbounded.
  explicit AsyncPriorityQueue(size_t max_size = 0) : max_size_(max_size) {}

  // --- Query ---

  [[nodiscard]] size_t Size() const { return items_.size(); }
  [[nodiscard]] bool Empty() const { return items_.empty(); }
  [[nodiscard]] bool Full() const {
    return max_size_ > 0 && items_.size() >= max_size_;
  }

  // --- Non-blocking operations ---

  /// Non-blocking put. Throws on full or shutdown.
  void PutNowait(T item, int priority = 0) {
    if (shutdown_) throw QueueShutDownError();
    if (max_size_ > 0 && items_.size() >= max_size_) {
      throw QueueFullError();
    }
    items_.emplace(priority, std::move(item));
    unfinished_tasks_++;
    NotifyWaiters();
  }

  /// Non-blocking get. Returns {item, priority}. Throws on empty or shutdown.
  std::pair<T, int> GetNowait() {
    if (items_.empty()) {
      if (shutdown_) throw QueueShutDownError();
      throw QueueEmptyError();
    }
    auto top_item = items_.top();
    int priority = top_item.priority;
    T item = std::move(top_item.item);
    items_.pop();
    return {std::move(item), priority};
  }

  // --- Async operations ---

  /// Adds an item with given priority. Lower value = higher priority.
  /// Blocks if bounded and full.
  Task<void> Put(T item, int priority = 0) {
    if (shutdown_) throw QueueShutDownError();

    // Clean up cancelled waiters.
    while (!waiters_.empty() && waiters_.front().Done()) {
      waiters_.pop_front();
    }

    if (!waiters_.empty()) {
      auto waiter = std::move(waiters_.front());
      waiters_.pop_front();
      items_.emplace(priority, std::move(item));
      unfinished_tasks_++;
      waiter.SetResult();
      co_return;
    }

    if (max_size_ > 0 && items_.size() >= max_size_) {
      // Wait for space.
      waiters_.emplace_back();
      try {
        co_await waiters_.back();
      } catch (...) {
        if (!waiters_.back().Done()) {
          waiters_.pop_back();
        }
        throw;
      }
    } else {
      items_.emplace(priority, std::move(item));
      unfinished_tasks_++;
    }
    co_return;
  }

  /// Removes and returns the highest priority item {item, priority}.
  /// Blocks if empty.
  Task<std::pair<T, int>> Get() {
    if (!items_.empty()) {
      auto top_item = items_.top();
      int priority = top_item.priority;
      T item = std::move(top_item.item);
      items_.pop();
      NotifyWaiters();
      co_return {std::move(item), priority};
    }

    if (shutdown_) throw QueueShutDownError();

    waiters_.emplace_back();
    try {
      co_await waiters_.back();
    } catch (...) {
      if (!items_.empty()) {
        auto top_item = items_.top();
        int priority = top_item.priority;
        T item = std::move(top_item.item);
        items_.pop();
        NotifyWaiters();
        co_return {std::move(item), priority};
      }
      throw;
    }

    if (items_.empty()) {
      if (shutdown_) throw QueueShutDownError();
      throw AsyncError("PriorityQueue Get() woke but no item available");
    }

    auto top_item = items_.top();
    int priority = top_item.priority;
    T item = std::move(top_item.item);
    items_.pop();
    NotifyWaiters();
    co_return {std::move(item), priority};
  }

  // --- Task-done / Join ---

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

  void Shutdown() {
    shutdown_ = true;
    for (auto& w : waiters_) {
      if (!w.Done()) {
        w.SetException(std::make_exception_ptr(QueueShutDownError()));
      }
    }
    waiters_.clear();
  }

 private:
  struct PriorityItem {
    bool operator<(const PriorityItem& other) const {
      // std::priority_queue is max-heap by default, so we invert for min-heap
      return other.priority < priority;
    }
    int priority;
    T item;
  };

  std::priority_queue<PriorityItem> items_;
  std::deque<Future<void>> waiters_;
  std::deque<Future<void>> join_waiters_;
  int unfinished_tasks_ = 0;
  bool shutdown_ = false;
  size_t max_size_;

  void NotifyWaiters() {
    while (!waiters_.empty() && waiters_.front().Done()) {
      waiters_.pop_front();
    }
    if (!waiters_.empty() && items_.size() < max_size_) {
      auto waiter = std::move(waiters_.front());
      waiters_.pop_front();
      waiter.SetResult();
    }
  }
};

}  // namespace asyncio

#endif  // ASYNCIO_SYNC_PRIORITY_QUEUE_H_
