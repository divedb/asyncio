// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Run() — top-level entry point, analogous to Python's asyncio.run().
// Runner — manages an EventLoop for multiple Run() calls.

#ifndef ASYNCIO_RUNNER_H_
#define ASYNCIO_RUNNER_H_

#include <functional>
#include <memory>
#include <utility>

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/policy.h"
#include "asyncio/task.h"

namespace asyncio {

/// Runs a coroutine until completion and returns its result.
///
/// Creates a new EventLoop, sets it as the current loop for the thread,
/// calls the coroutine factory to create the Task (which starts the
/// coroutine body with the correct loop in scope), runs the event loop
/// until the Task completes, and then cleans up.
///
/// This is the top-level entry point, analogous to Python's `asyncio.run()`.
///
/// \param coro_factory  A callable that returns a Task<T>.
/// \return              The result of the coroutine.
template <typename T>
T Run(Task<T> (*coro_factory)()) {
  EventLoop loop;

  // Set this loop as current BEFORE the coroutine starts.
  EventLoop* prev = EventLoop::Current();
  EventLoop::SetCurrent(&loop);

  // Set this loop as the "running" loop so that GetRunningLoop() works
  // inside the coroutine body (before RunForever() is called).
  EventLoopPolicy::SetRunningLoop(&loop);

  // Create the task (this starts the coroutine body).
  Task<T> task = coro_factory();

  // Arrange to stop the loop when the task completes.
  task.AddDoneCallback([&loop](Future<T>&) { loop.Stop(); });

  // If the task did not complete synchronously, run the loop.
  if (!task.Done()) {
    loop.RunForever();  // RunForever also sets/clears the running loop.
  }

  // Clear the running loop marker.
  EventLoopPolicy::SetRunningLoop(nullptr);

  // Restore the previous current loop (before loop is destroyed).
  EventLoop::SetCurrent(prev);

  // Propagate any exception stored in the Task/Future.
  return task.Result();
}

/// Overload for generic callable (lambdas, std::function, etc.).
/// The return type is auto, so callers can use:
///   auto result = asyncio::Run([&]() -> Task<int> { ... });
/// or:
///   asyncio::Run<void>([&]() -> Task<void> { ... });
template <typename T, typename Factory>
T Run(Factory&& coro_factory) {
  EventLoop loop;

  // Set this loop as current BEFORE the coroutine starts.
  EventLoop* prev = EventLoop::Current();
  EventLoop::SetCurrent(&loop);

  // Set this loop as the "running" loop so that GetRunningLoop() works
  // inside the coroutine body (before RunForever() is called).
  EventLoopPolicy::SetRunningLoop(&loop);

  // Create the task (this starts the coroutine body).
  Task<T> task = coro_factory();

  // Arrange to stop the loop when the task completes.
  task.AddDoneCallback([&loop](Future<T>&) { loop.Stop(); });

  // If the task did not complete synchronously, run the loop.
  if (!task.Done()) {
    loop.RunForever();  // RunForever also sets/clears the running loop.
  }

  // Clear the running loop marker.
  EventLoopPolicy::SetRunningLoop(nullptr);

  // Restore the previous current loop (before loop is destroyed).
  EventLoop::SetCurrent(prev);

  // Propagate any exception stored in the Task/Future.
  return task.Result();
}

/// Runner manages an EventLoop for multiple Run() calls.
///
/// Analogous to Python 3.11+ `asyncio.Runner`.
/// The same Runner can be used to run multiple coroutines sequentially,
/// reusing the same underlying EventLoop.
class Runner {
 public:
  /// Creates a Runner.
  ///
  /// \param debug  Unused; kept for API compatibility with Python.
  explicit Runner(bool debug = false)
      : loop_(std::make_unique<EventLoop>()), debug_(debug) {
    EventLoop::SetCurrent(loop_.get());
  }

  /// Closes the managed EventLoop and releases resources.
  ~Runner() { Close(); }

  /// Runs a coroutine on the managed EventLoop and returns its result.
  ///
  /// \param coro_factory  A callable that returns a Task<T>.
  /// \return              The result of the coroutine.
  template <typename T>
  T Run(Task<T> (*coro_factory)()) {
    // Create the task (this starts the coroutine body).
    Task<T> task = coro_factory();

    // Arrange to stop the loop when the task completes.
    task.AddDoneCallback([this](Future<T>&) { loop_->Stop(); });

    // If the task did not complete synchronously, run the loop.
    if (!task.Done()) {
      loop_->RunForever();
    }

    return task.Result();
  }

  /// Overload for generic callable (lambdas, etc.).
  template <typename T, typename Factory>
  T Run(Factory&& coro_factory) {
    // Create the task (this starts the coroutine body).
    Task<T> task = coro_factory();

    // Arrange to stop the loop when the task completes.
    task.AddDoneCallback([this](Future<T>&) { loop_->Stop(); });

    // If the task did not complete synchronously, run the loop.
    if (!task.Done()) {
      loop_->RunForever();
    }

    return task.Result();
  }

  /// Closes the managed EventLoop.
  ///
  /// After calling Close(), the Runner is no longer usable until a new
  /// EventLoop is created (by destroying and re-creating the Runner).
  void Close() { loop_.reset(); }

  /// Returns a pointer to the managed EventLoop, or nullptr if closed.
  [[nodiscard]] EventLoop* GetLoop() { return loop_.get(); }

  // Non-copyable.
  Runner(const Runner&) = delete;
  Runner& operator=(const Runner&) = delete;

 private:
  std::unique_ptr<EventLoop> loop_;
  [[maybe_unused]] bool debug_;
};

}  // namespace asyncio

#endif  // ASYNCIO_RUNNER_H_
