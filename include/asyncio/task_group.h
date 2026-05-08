// Copyright 2025 asyncio-cpp authors. All rights reserved.
// TaskGroup — structured concurrency for managing groups of tasks.
// Analogous to Python 3.11+ asyncio.TaskGroup.

#ifndef ASYNCIO_TASK_GROUP_H_
#define ASYNCIO_TASK_GROUP_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "asyncio/error.h"
#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/task.h"

namespace asyncio {

// ---------------------------------------------------------------------------
// TaskGroup — manages a group of tasks with structured concurrency.
// ---------------------------------------------------------------------------

/// A group of tasks that run concurrently within a structured scope.
///
/// TaskGroup provides structured concurrency, analogous to Python 3.11+
/// `asyncio.TaskGroup`.
///
/// Key behaviors:
/// - Creating a child task via `CreateTask()` adds it to the group's task list.
/// - When any child task completes with an exception, all other child tasks
///   are cancelled automatically.
/// - `WaitComplete()` waits for all child tasks to finish and re-raises
///   the first exception encountered (if any).
///
/// Example:
///   TaskGroup group;
///   group.CreateTask(myCoroutine1());  // Starts immediately
///   group.CreateTask(myCoroutine2());  // Starts immediately
///   co_await group.WaitComplete();      // Wait for both to finish
///
class TaskGroup {
 public:
  /// Creates a TaskGroup.
  TaskGroup();

  /// Destroys the TaskGroup. If any child tasks are still running, they are
  /// cancelled.
  ~TaskGroup();

  // Non-copyable, non-movable.
  TaskGroup(const TaskGroup&) = delete;
  TaskGroup& operator=(const TaskGroup&) = delete;
  TaskGroup(TaskGroup&&) = delete;
  TaskGroup& operator=(TaskGroup&&) = delete;

  /// Creates and starts a child task within this group.
  ///
  /// The task starts executing immediately (eager start).
  /// If any previously created child task raised an exception, the new task
  /// is cancelled immediately.
  ///
  /// \param coro  A coroutine that returns Task<T>.
  /// \return      The created Task<T>.
  template <typename T>
  Task<T> CreateTask(Task<T> coro);

  /// Waits for all child tasks to complete.
  ///
  /// If any child task raised an exception, the first exception is re-thrown.
  /// This must be called from a coroutine context.
  ///
  /// \return A Task<void> that completes when all child tasks finish.
  Task<void> WaitComplete();

  /// Cancels all child tasks.
  void CancelAll();

  /// Returns the number of child tasks that have been created.
  int TaskCount() const;

  /// Returns the number of child tasks that have completed.
  int CompletedCount() const;

  /// Returns true if an exception was raised by a child task.
  bool HasException() const;

  /// Returns the first exception, or nullptr if none.
  std::exception_ptr GetException() const;

 private:
  /// Internal shared state.
  struct SharedState {
    /// Cancel functions for each task.
    std::vector<std::function<void()>> cancel_funcs;

    /// Count of child tasks created.
    std::atomic<int> total_count{0};

    /// Count of child tasks completed.
    std::atomic<int> completed_count{0};

    /// The first exception encountered.
    std::exception_ptr exception;

    /// True if an exception has been encountered.
    std::atomic<bool> has_exception{false};

    /// Completion future signaled when all tasks finish.
    Future<void> completion_future;

    /// Mutex protecting cancel_funcs and exception.
    std::mutex mutex;
  };

  std::shared_ptr<SharedState> state_;
};

// ---------------------------------------------------------------------------
// TaskGroup implementation.
// ---------------------------------------------------------------------------

inline TaskGroup::TaskGroup() : state_(std::make_shared<SharedState>()) {}

inline TaskGroup::~TaskGroup() { CancelAll(); }

namespace detail {
/// Helper to create a cancel wrapper that holds a task by value.
template <typename T>
struct TaskCancelWrapper {
  Task<T> task;
  void Cancel() { task.Cancel(); }
};
}  // namespace detail

template <typename T>
Task<T> TaskGroup::CreateTask(Task<T> coro) {
  // If the group already has an exception, cancel immediately.
  if (state_->has_exception) {
    coro.Cancel();
    return coro;
  }

  // Wrap the task in a shared_ptr so the cancel function can hold a copy.
  auto wrapper = std::make_shared<detail::TaskCancelWrapper<T>>(
      detail::TaskCancelWrapper<T>{std::move(coro)});
  auto self = state_;

  // Register a completion callback.
  wrapper->task.AddDoneCallback([self, wrapper](Future<T>&) {
    self->completed_count++;

    // Check if this task raised an exception.
    std::exception_ptr ex = wrapper->task.GetException();
    if (ex) {
      // Atomically check and set the first exception.
      bool we_set_exception = false;
      {
        std::lock_guard<std::mutex> lock(self->mutex);
        if (!self->has_exception) {
          self->has_exception = true;
          self->exception = ex;
          we_set_exception = true;
        }
      }

      // If we set the first exception, cancel all other pending tasks.
      if (we_set_exception) {
        std::lock_guard<std::mutex> lock(self->mutex);
        for (auto& cancel_fn : self->cancel_funcs) {
          cancel_fn();
        }
      }
    }

    // Check if all tasks have completed.
    if (self->completed_count >= self->total_count) {
      std::lock_guard<std::mutex> lock(self->mutex);
      if (!self->completion_future.Done()) {
        if (self->exception) {
          self->completion_future.SetException(self->exception);
        } else {
          self->completion_future.SetResult();
        }
      }
    }
  });

  // Store cancel function.
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->cancel_funcs.push_back([wrapper]() { wrapper->Cancel(); });
    state_->total_count++;
  }

  return wrapper->task;
}

inline Task<void> TaskGroup::WaitComplete() {
  // Check if already complete.
  if (state_->completed_count >= state_->total_count) {
    if (state_->exception) {
      std::rethrow_exception(state_->exception);
    }
    co_return;
  }

  // co_await the completion future.
  co_await state_->completion_future;

  // Re-throw exception if any.
  if (state_->exception) {
    std::rethrow_exception(state_->exception);
  }
}

inline void TaskGroup::CancelAll() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  for (auto& cancel_fn : state_->cancel_funcs) {
    cancel_fn();
  }
}

inline int TaskGroup::TaskCount() const { return state_->total_count; }

inline int TaskGroup::CompletedCount() const { return state_->completed_count; }

inline bool TaskGroup::HasException() const { return state_->has_exception; }

inline std::exception_ptr TaskGroup::GetException() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->exception;
}

}  // namespace asyncio

#endif  // ASYNCIO_TASK_GROUP_H_
