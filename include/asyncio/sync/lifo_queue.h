// Copyright 2025 asyncio-cpp authors. All rights reserved.
// lifo_queue.h — LIFO (stack) queue for asyncio.

#ifndef ASYNCIO_SYNC_LIFO_QUEUE_H_
#define ASYNCIO_SYNC_LIFO_QUEUE_H_

#include <stack>

#include "asyncio/sync/queue.h"

namespace asyncio {

/// A LIFO (Last-In, First-Out) queue for producer-consumer patterns.
/// Items are retrieved in reverse order of insertion (stack behavior).
///
/// Mirrors Python's `asyncio.LifoQueue`.
template <typename T>
class AsyncLifoQueue {
 public:
  /// Creates a LIFO queue.
  /// If max_size > 0, the queue is bounded. Default is unbounded.
  explicit AsyncLifoQueue(size_t max_size = 0) : max_size_(max_size) {}

  // --- Query ---

  [[nodiscard]] size_t Size() const { return items_.size(); }
  [[nodiscard]] bool Empty() const { return items_.empty(); }
  [[nodiscard]] bool Full() const {
    return max_size_ > 0 && items_.size() >= max_size_;
  }

  // --- Non-blocking operations ---

  /// Non-blocking put. Throws on full or shutdown.
  void PutNowait(T item) {
    if (shutdown_) throw QueueShutDownError();
    if (max_size_ > 0 && items_.size() >= max_size_) {
      throw QueueFullError();
    }
    items_.push(std::move(item));
    unfinished_tasks_++;
    NotifyWaiters();
  }

  /// Non-blocking get. Throws on empty or shutdown.
  T GetNowait() {
    if (items_.empty()) {
      if (shutdown_) throw QueueShutDownError();
      throw QueueEmptyError();
    }
    T item = std::move(items_.top());
    items_.pop();
    NotifyWaiters();
    return item;
  }

  // --- Async operations ---

  /// Adds an item to the top of the stack.
  /// Blocks if bounded and full.
  Task<void> Put(T item) {
    if (shutdown_) throw QueueShutDownError();

    // Clean up cancelled waiters.
    while (!waiters_.empty() && waiters_.front().Done()) {
      waiters_.pop_front();
    }

    if (!waiters_.empty()) {
      auto waiter = std::move(waiters_.front());
      waiters_.pop_front();
      items_.push(std::move(item));
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
      items_.push(std::move(item));
      unfinished_tasks_++;
    }
    co_return;
  }

  /// Removes and returns the most recently added item (LIFO).
  /// Blocks if empty.
  Task<T> Get() {
    if (!items_.empty()) {
      T item = std::move(items_.top());
      items_.pop();
      NotifyWaiters();
      co_return item;
    }

    if (shutdown_) throw QueueShutDownError();

    waiters_.emplace_back();
    try {
      co_await waiters_.back();
    } catch (...) {
      if (!items_.empty()) {
        T item = std::move(items_.top());
        items_.pop();
        NotifyWaiters();
        co_return item;
      }
      throw;
    }

    if (items_.empty()) {
      if (shutdown_) throw QueueShutDownError();
      throw AsyncError("LifoQueue Get() woke but no item available");
    }

    T item = std::move(items_.top());
    items_.pop();
    NotifyWaiters();
    co_return item;
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
  std::stack<T> items_;
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

#endif  // ASYNCIO_SYNC_LIFO_QUEUE_H_
