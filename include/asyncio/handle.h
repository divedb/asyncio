// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Handle — type-erased callback wrapper with cooperative cancellation.

#ifndef ASYNCIO_HANDLE_H_
#define ASYNCIO_HANDLE_H_

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace asyncio {

/// A type-erased callback that can be scheduled on the event loop.
///
/// Handles are the fundamental unit of work in the event loop. Each Handle
/// wraps a nullary callback and supports cooperative cancellation: once
/// Cancel() is called, Run() becomes a no-op.
///
/// Internally, Handle uses shared state so that both the event loop and
/// the caller hold independent references to the same logical handle.
/// Cancel() on any copy cancels all of them.
///
/// This mirrors Python's `asyncio.Handle`.
class Handle {
 public:
  /// Constructs a Handle wrapping the given callback.
  explicit Handle(std::function<void()> callback);

  /// Default construct produces a null handle (not schedulable).
  Handle() = default;

  /// Copyable — both copies share the same underlying state.
  Handle(const Handle&) = default;
  Handle& operator=(const Handle&) = default;

  /// Move-constructible.
  Handle(Handle&& other) noexcept = default;
  Handle& operator=(Handle&& other) noexcept = default;

  /// Executes the callback unless this handle has been cancelled.
  /// After cancellation, Run() is a no-op.
  void Run();

  /// Requests cooperative cancellation. After this call, Run() will be a
  /// no-op. Returns true if this handle was not already cancelled.
  /// Thread-safe: uses an atomic flag.
  bool Cancel();

  /// Returns true if Cancel() has been called.
  [[nodiscard]] bool Cancelled() const;

  /// Returns true if this Handle holds a valid (non-null) state.
  [[nodiscard]] bool Valid() const;

 private:
  struct State {
    std::function<void()> callback;
    std::atomic<bool> cancelled{false};
  };

  explicit Handle(std::shared_ptr<State> state) : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

}  // namespace asyncio

#endif  // ASYNCIO_HANDLE_H_
