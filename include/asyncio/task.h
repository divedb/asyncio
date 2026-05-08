// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Task<T> — a coroutine-driving Future with cooperative cancellation.

#ifndef ASYNCIO_TASK_H_
#define ASYNCIO_TASK_H_

#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "asyncio/event_loop.h"
#include "asyncio/future.h"

namespace asyncio {

// Forward declarations.
template <typename T>
class Task;

template <typename T>
struct AsyncTaskPromise;

// ---------------------------------------------------------------------------
// TaskState — shared state for cancellation tracking.
// ---------------------------------------------------------------------------

struct TaskState {
  std::coroutine_handle<> coro;
  bool must_cancel = false;
  int cancel_count = 0;
  std::string name;
  /// Type-erased function to cancel the Future currently being awaited.
  std::function<void()> cancel_awaited_fn;

  ~TaskState() {
    if (coro) {
      coro.destroy();
    }
  }
};

// ---------------------------------------------------------------------------
// Task<T> — coroutine-driving Future (non-void specialization).
// ---------------------------------------------------------------------------

template <typename T>
class Task : public Future<T> {
 public:
  using inner_type = T;
  using promise_type = AsyncTaskPromise<T>;

  Task() = default;

  Task(const Task&) = default;
  Task& operator=(const Task&) = default;
  Task(Task&&) noexcept = default;
  Task& operator=(Task&&) noexcept = default;

  /// Starts the coroutine (for lazy-start coroutines).
  /// With initial_suspend = suspend_always, the coroutine is created
  /// suspended; call Start() to begin execution.
  void Start() {
    if (task_state_ && task_state_->coro) {
      task_state_->coro.resume();
    }
  }

  /// Requests cooperative cancellation. Cancels the awaited Future, then
  /// cancels this Task's Future. Returns false if already done.
  bool Cancel() {
    if (this->Done()) return false;
    if (!task_state_) return false;
    task_state_->must_cancel = true;
    task_state_->cancel_count++;

    // Cancel the Task's Future (transitions to CANCELLED, fires done
    // callbacks so any awaiter of this Task is notified).
    Future<T>::Cancel();

    // Cancel whatever the coroutine is currently awaiting, which causes the
    // coroutine to resume with AsyncCancelledError.
    if (task_state_->cancel_awaited_fn) {
      auto fn = std::move(task_state_->cancel_awaited_fn);
      task_state_->cancel_awaited_fn = nullptr;
      fn();
    }
    return true;
  }

  /// Returns the current cancellation depth. Mirrors Python 3.11+
  /// Task.cancelling().
  [[nodiscard]] int Cancelling() const {
    return task_state_ ? task_state_->cancel_count : 0;
  }

  /// Decrements the cancellation depth. If it reaches zero, clears
  /// must_cancel. Mirrors Python 3.11+ Task.uncancel().
  void Uncancel() {
    if (!task_state_) return;
    if (task_state_->cancel_count > 0) {
      task_state_->cancel_count--;
    }
    if (task_state_->cancel_count == 0) {
      task_state_->must_cancel = false;
    }
  }

  [[nodiscard]] std::string GetName() const {
    return task_state_ ? task_state_->name : "";
  }

  void SetName(std::string name) {
    if (task_state_) task_state_->name = std::move(name);
  }

  /// Returns the task state (used by await_transform to start coroutines).
  std::shared_ptr<TaskState>& GetTaskState() { return task_state_; }

 private:
  friend struct AsyncTaskPromise<T>;

  std::shared_ptr<TaskState> task_state_;
};

// ---------------------------------------------------------------------------
// Task<void> — void specialization.
// ---------------------------------------------------------------------------

template <>
class Task<void> : public Future<void> {
 public:
  using inner_type = void;
  using promise_type = AsyncTaskPromise<void>;

  Task() = default;
  Task(const Task&) = default;
  Task& operator=(const Task&) = default;
  Task(Task&&) noexcept = default;
  Task& operator=(Task&&) noexcept = default;

  /// Starts the coroutine (for lazy-start coroutines).
  /// With initial_suspend = suspend_always, the coroutine is created
  /// suspended; call Start() to begin execution.
  void Start() {
    if (task_state_ && task_state_->coro) {
      task_state_->coro.resume();
    }
  }

  bool Cancel() {
    if (this->Done()) return false;
    if (!task_state_) return false;
    task_state_->must_cancel = true;
    task_state_->cancel_count++;
    Future<void>::Cancel();
    if (task_state_->cancel_awaited_fn) {
      auto fn = std::move(task_state_->cancel_awaited_fn);
      task_state_->cancel_awaited_fn = nullptr;
      fn();
    }
    return true;
  }

  [[nodiscard]] int Cancelling() const {
    return task_state_ ? task_state_->cancel_count : 0;
  }

  void Uncancel() {
    if (!task_state_) return;
    if (task_state_->cancel_count > 0) {
      task_state_->cancel_count--;
    }
    if (task_state_->cancel_count == 0) {
      task_state_->must_cancel = false;
    }
  }

  [[nodiscard]] std::string GetName() const {
    return task_state_ ? task_state_->name : "";
  }

  void SetName(std::string name) {
    if (task_state_) task_state_->name = std::move(name);
  }

  /// Returns the task state (used by await_transform to start coroutines).
  std::shared_ptr<TaskState>& GetTaskState() { return task_state_; }

 private:
  friend struct AsyncTaskPromise<void>;

  std::shared_ptr<TaskState> task_state_;
};

// ---------------------------------------------------------------------------
// AsyncTaskPromise<T> — promise type for coroutines returning Task<T>.
// ---------------------------------------------------------------------------

template <typename T>
struct AsyncTaskPromise {
  /// The promise's copy of the Task. Shares state with the returned Task.
  Task<T> task_;

  Task<T> get_return_object() {
    task_.task_state_ = std::make_shared<TaskState>();
    task_.task_state_->coro =
        std::coroutine_handle<AsyncTaskPromise>::from_promise(*this);
    return task_;  // Copy — shared state.
  }

  /// Eager start: the coroutine body begins executing immediately when the Task
  /// is created. The first co_await triggers the await_transform machinery.
  /// This matches Python's eager coroutine semantics.
  std::suspend_never initial_suspend() noexcept { return {}; }

  auto final_suspend() noexcept {
    struct FinalAwaiter {
      std::shared_ptr<TaskState> task_state;

      [[nodiscard]] bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> h) noexcept {
        // Null out the coroutine handle so TaskState's destructor won't
        // try to destroy the coroutine after the deferred callback runs.
        task_state->coro = nullptr;

        // Schedule deferred destruction. We cannot destroy the coroutine
        // while inside it, so we post a callback to destroy it later.
        if (auto* loop = EventLoop::Current()) {
          loop->CallSoon([h, state = std::move(task_state)]() mutable {
            state->coro = nullptr;  // Prevent TaskState from double-destroying
            h.destroy();
          });
        }
      }

      void await_resume() const noexcept {}
    };

    auto state = task_.task_state_;
    task_ = Task<T>();  // Clear the promise's Task reference
    return FinalAwaiter{state};
  }

  void return_value(T value) {
    if (!task_.Done()) {
      task_.SetResult(std::move(value));
    }
  }

  void unhandled_exception() {
    if (!task_.Done()) {
      task_.SetException(std::current_exception());
    }
  }

  // --- await_transform: intercept co_await of Future types ---

  template <typename U>
  auto await_transform(Future<U> fut) {
    struct FutureAwaiter {
      Future<U> future;
      std::shared_ptr<TaskState> task_state;

      [[nodiscard]] bool await_ready() { return future.Done(); }

      void await_suspend(std::coroutine_handle<> caller) {
        // Register a cancel function so Task::Cancel() can cancel this
        // Future, which will resume the coroutine with AsyncCancelledError.
        task_state->cancel_awaited_fn = [this]() {
          future.Cancel();
        };
        future.AddDoneCallback([caller](Future<U>&) { caller.resume(); });
      }

      auto await_resume() {
        task_state->cancel_awaited_fn = nullptr;
        if constexpr (std::is_void_v<U>) {
          future.Result();  // Validates state, throws on error.
        } else {
          return std::move(future.Result());
        }
      }
    };

    return FutureAwaiter{std::move(fut), task_.task_state_};
  }

  // --- await_transform: intercept co_await of Task types ---
  // When an outer coroutine co_awaits an inner Task<U>, the inner coroutine
  // has already started (eager start). We just need to register the continuation.

  template <typename U>
  auto await_transform(Task<U> task) {
    struct TaskAwaiter {
      Task<U> task;
      std::shared_ptr<TaskState> task_state;

      [[nodiscard]] bool await_ready() { return task.Done(); }

      void await_suspend(std::coroutine_handle<> caller) {
        // Register a cancel function so Task::Cancel() can cancel this task.
        task_state->cancel_awaited_fn = [this]() {
          task.Cancel();
        };

        // When the task completes, resume the caller coroutine.
        task.AddDoneCallback([caller](Future<U>&) { caller.resume(); });
      }

      auto await_resume() {
        task_state->cancel_awaited_fn = nullptr;
        if constexpr (std::is_void_v<U>) {
          task.Result();  // Validates state, throws on error.
        } else {
          return std::move(task.Result());
        }
      }
    };

    return TaskAwaiter{std::move(task), task_.task_state_};
  }

  /// Pass-through for non-Future awaitables.
  template <typename A>
  auto await_transform(A&& a) {
    return std::forward<A>(a);
  }
};

// ---------------------------------------------------------------------------
// AsyncTaskPromise<void> — void specialization.
// ---------------------------------------------------------------------------

template <>
struct AsyncTaskPromise<void> {
  Task<void> task_;

  Task<void> get_return_object() {
    task_.task_state_ = std::make_shared<TaskState>();
    task_.task_state_->coro =
        std::coroutine_handle<AsyncTaskPromise>::from_promise(*this);
    return task_;
  }

  /// Eager start: the coroutine body begins executing immediately when the Task
  /// is created. The first co_await triggers the await_transform machinery.
  /// This matches Python's eager coroutine semantics.
  std::suspend_never initial_suspend() noexcept { return {}; }

  auto final_suspend() noexcept {
    struct FinalAwaiter {
      std::shared_ptr<TaskState> task_state;

      [[nodiscard]] bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> h) noexcept {
        task_state->coro = nullptr;
        if (auto* loop = EventLoop::Current()) {
          loop->CallSoon([h, state = std::move(task_state)]() mutable {
            state->coro = nullptr;
            h.destroy();
          });
        }
      }

      void await_resume() const noexcept {}
    };

    auto state = task_.task_state_;
    task_ = Task<void>();
    return FinalAwaiter{state};
  }

  void return_void() {
    if (!task_.Done()) {
      task_.SetResult();
    }
  }

  void unhandled_exception() {
    if (!task_.Done()) {
      task_.SetException(std::current_exception());
    }
  }

  // --- await_transform ---

  template <typename U>
  auto await_transform(Future<U> fut) {
    struct FutureAwaiter {
      Future<U> future;
      std::shared_ptr<TaskState> task_state;

      [[nodiscard]] bool await_ready() { return future.Done(); }

      void await_suspend(std::coroutine_handle<> caller) {
        task_state->cancel_awaited_fn = [this]() { future.Cancel(); };
        future.AddDoneCallback([caller](Future<U>&) { caller.resume(); });
      }

      auto await_resume() {
        task_state->cancel_awaited_fn = nullptr;
        if constexpr (std::is_void_v<U>) {
          future.Result();
        } else {
          return std::move(future.Result());
        }
      }
    };

    return FutureAwaiter{std::move(fut), task_.task_state_};
  }

  // --- await_transform: intercept co_await of Task types ---
  // When an outer coroutine co_awaits an inner Task<U>, the inner coroutine
  // has already started (eager start). We just need to register the continuation.

  template <typename U>
  auto await_transform(Task<U> task) {
    struct TaskAwaiter {
      Task<U> task;
      std::shared_ptr<TaskState> task_state;

      [[nodiscard]] bool await_ready() { return task.Done(); }

      void await_suspend(std::coroutine_handle<> caller) {
        // Register a cancel function.
        task_state->cancel_awaited_fn = [this]() {
          task.Cancel();
        };

        // When the task completes, resume the caller coroutine.
        task.AddDoneCallback([caller](Future<U>&) { caller.resume(); });
      }

      auto await_resume() {
        task_state->cancel_awaited_fn = nullptr;
        if constexpr (std::is_void_v<U>) {
          task.Result();
        } else {
          return std::move(task.Result());
        }
      }
    };

    return TaskAwaiter{std::move(task), task_.task_state_};
  }

  template <typename A>
  auto await_transform(A&& a) {
    return std::forward<A>(a);
  }
};

// ---------------------------------------------------------------------------
// CreateTask — factory function.
// ---------------------------------------------------------------------------

/// Creates a Task from a coroutine. The coroutine starts eagerly.
template <typename T>
Task<T> CreateTask(Task<T> task) {
  return task;
}

}  // namespace asyncio

#endif  // ASYNCIO_TASK_H_
