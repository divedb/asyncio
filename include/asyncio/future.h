// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Future<T> — shared-state result container with coroutine awaitable support.

#ifndef ASYNCIO_FUTURE_H_
#define ASYNCIO_FUTURE_H_

#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "asyncio/error.h"

namespace asyncio {

/// A shared-state container that represents the eventual result of an
/// asynchronous operation.
///
/// Future<T> models a state machine:
///   PENDING → FINISHED (result or exception) or CANCELLED
///
/// Transitions are one-way. Once resolved or cancelled, the Future is
/// immutable.
///
/// Internally, Future uses shared state so that multiple references to the
/// same logical Future share state. This mirrors Python's asyncio.Future,
/// which is a reference type.
///
/// Example:
///   Future<int> fut;
///   fut.SetResult(42);
///   EXPECT_EQ(fut.Result(), 42);
///
///   Future<int> fut2;
///   auto copy = fut2;  // Both share the same state.
///   copy.SetResult(7);
///   EXPECT_EQ(fut2.Result(), 7);
template <typename T>
class Future {
 public:
  /// Constructs a Future in the Pending state.
  Future() : state_(std::make_shared<SharedState>()) {}

  /// Copyable — both copies share the same underlying state.
  Future(const Future&) = default;
  Future& operator=(const Future&) = default;

  /// Movable.
  Future(Future&&) noexcept = default;
  Future& operator=(Future&&) noexcept = default;

  /// Returns true if the Future is resolved (finished or cancelled).
  [[nodiscard]] bool Done() const {
    return state_->state != State::kPending;
  }

  /// Returns true if the Future was cancelled.
  [[nodiscard]] bool Cancelled() const {
    return state_->state == State::kCancelled;
  }

  /// Returns the stored result. Throws InvalidStateError if not done,
  /// AsyncCancelledError if cancelled, or rethrows the stored exception.
  T& Result() {
    ValidateAndGet();
    return state_->result.value();
  }

  /// Returns the stored result (const overload).
  const T& Result() const {
    ValidateAndGet();
    return state_->result.value();
  }

  /// Returns the stored exception, or nullptr if none.
  [[nodiscard]] std::exception_ptr GetException() const {
    return state_->exception;
  }

  /// Resolves the Future with a value. Transitions PENDING → FINISHED.
  /// Throws InvalidStateError if already resolved or cancelled.
  void SetResult(T value) {
    if (state_->state != State::kPending) {
      throw InvalidStateError("Result already set");
    }
    state_->state = State::kFinished;
    state_->result = std::move(value);
    InvokeCallbacks();
  }

  /// Resolves the Future with an exception. Transitions PENDING → FINISHED.
  /// Throws InvalidStateError if already resolved or cancelled.
  void SetException(std::exception_ptr ex) {
    if (state_->state != State::kPending) {
      throw InvalidStateError("Result already set");
    }
    state_->state = State::kFinished;
    state_->exception = std::move(ex);
    InvokeCallbacks();
  }

  /// Cancels the Future. Transitions PENDING → CANCELLED.
  /// Returns true if the cancellation succeeded (state was PENDING).
  /// Returns false if already resolved or cancelled.
  bool Cancel() {
    if (state_->state != State::kPending) return false;
    state_->state = State::kCancelled;
    InvokeCallbacks();
    return true;
  }

  /// Registers a callback to be invoked when the Future resolves.
  /// If the Future is already done, the callback is invoked immediately.
  void AddDoneCallback(std::function<void(Future<T>&)> callback) {
    if (Done()) {
      callback(*this);
      return;
    }
    state_->done_callbacks.push_back(std::move(callback));
  }

  /// Returns an Awaiter for use with co_await.
  /// Suspends the calling coroutine until the Future is done, then returns
  /// the result (or rethrows the exception).
  auto operator co_await() {
    struct Awaiter {
      Future<T>& future;

      [[nodiscard]] bool await_ready() const { return future.Done(); }

      void await_suspend(std::coroutine_handle<> caller) {
        future.AddDoneCallback(
            [caller](Future<T>&) { caller.resume(); });
      }

      T await_resume() { return std::move(future.Result()); }
    };
    return Awaiter{*this};
  }

 private:
  enum class State { kPending, kFinished, kCancelled };

  struct SharedState {
    State state = State::kPending;
    std::optional<T> result;
    std::exception_ptr exception;
    std::vector<std::function<void(Future<T>&)>> done_callbacks;
  };

  std::shared_ptr<SharedState> state_;

  /// Validates the Future state and throws an appropriate exception
  /// if the Future is not in the FINISHED state with a result.
  void ValidateAndGet() const {
    if (state_->state == State::kPending) {
      throw InvalidStateError("Future is not done");
    }
    if (state_->state == State::kCancelled) {
      throw AsyncCancelledError();
    }
    if (state_->exception) {
      std::rethrow_exception(state_->exception);
    }
  }

  /// Invokes all registered done callbacks and clears the callback list.
  void InvokeCallbacks() {
    auto callbacks = std::move(state_->done_callbacks);
    state_->done_callbacks.clear();
    for (auto& cb : callbacks) {
      cb(*this);
    }
  }
};

/// Specialization of Future for void (no result value).
///
/// Used for operations that complete but don't produce a value, such as
/// Sleep(), Yield(), or AsyncLock::Acquire().
template <>
class Future<void> {
 public:
  /// Constructs a Future<void> in the Pending state.
  Future() : state_(std::make_shared<SharedState>()) {}

  /// Copyable — both copies share the same underlying state.
  Future(const Future&) = default;
  Future& operator=(const Future&) = default;

  /// Movable.
  Future(Future&&) noexcept = default;
  Future& operator=(Future&&) noexcept = default;

  /// Returns true if the Future is resolved (finished or cancelled).
  [[nodiscard]] bool Done() const {
    return state_->state != State::kPending;
  }

  /// Returns true if the Future was cancelled.
  [[nodiscard]] bool Cancelled() const {
    return state_->state == State::kCancelled;
  }

  /// Validates the Future state. Throws if not done, cancelled, or has
  /// an exception. Equivalent to Result() for non-void Futures.
  void Result() const {
    if (state_->state == State::kPending) {
      throw InvalidStateError("Future is not done");
    }
    if (state_->state == State::kCancelled) {
      throw AsyncCancelledError();
    }
    if (state_->exception) {
      std::rethrow_exception(state_->exception);
    }
  }

  /// Returns the stored exception, or nullptr if none.
  [[nodiscard]] std::exception_ptr GetException() const {
    return state_->exception;
  }

  /// Resolves the Future with no value. Transitions PENDING → FINISHED.
  /// Throws InvalidStateError if already resolved or cancelled.
  void SetResult() {
    if (state_->state != State::kPending) {
      throw InvalidStateError("Result already set");
    }
    state_->state = State::kFinished;
    InvokeCallbacks();
  }

  /// Resolves the Future with an exception. Transitions PENDING → FINISHED.
  /// Throws InvalidStateError if already resolved or cancelled.
  void SetException(std::exception_ptr ex) {
    if (state_->state != State::kPending) {
      throw InvalidStateError("Result already set");
    }
    state_->state = State::kFinished;
    state_->exception = std::move(ex);
    InvokeCallbacks();
  }

  /// Cancels the Future. Transitions PENDING → CANCELLED.
  /// Returns true if the cancellation succeeded (state was PENDING).
  bool Cancel() {
    if (state_->state != State::kPending) return false;
    state_->state = State::kCancelled;
    InvokeCallbacks();
    return true;
  }

  /// Registers a callback to be invoked when the Future resolves.
  /// If the Future is already done, the callback is invoked immediately.
  void AddDoneCallback(std::function<void(Future<void>&)> callback) {
    if (Done()) {
      callback(*this);
      return;
    }
    state_->done_callbacks.push_back(std::move(callback));
  }

  /// Returns an Awaiter for use with co_await.
  auto operator co_await() {
    struct Awaiter {
      Future<void>& future;

      [[nodiscard]] bool await_ready() const { return future.Done(); }

      void await_suspend(std::coroutine_handle<> caller) {
        future.AddDoneCallback(
            [caller](Future<void>&) { caller.resume(); });
      }

      void await_resume() { future.Result(); }
    };
    return Awaiter{*this};
  }

 private:
  enum class State { kPending, kFinished, kCancelled };

  struct SharedState {
    State state = State::kPending;
    std::exception_ptr exception;
    std::vector<std::function<void(Future<void>&)>> done_callbacks;
  };

  std::shared_ptr<SharedState> state_;

  /// Invokes all registered done callbacks and clears the callback list.
  void InvokeCallbacks() {
    auto callbacks = std::move(state_->done_callbacks);
    state_->done_callbacks.clear();
    for (auto& cb : callbacks) {
      cb(*this);
    }
  }
};

}  // namespace asyncio

#endif  // ASYNCIO_FUTURE_H_
