// Copyright 2025 asyncio-cpp authors. All rights reserved.
// EventLoopPolicy — abstraction layer for event loop creation and management.
// Analogous to Python's asyncio.AbstractEventLoopPolicy.

#ifndef ASYNCIO_POLICY_H_
#define ASYNCIO_POLICY_H_

#include <memory>
#include <mutex>

namespace asyncio {

class EventLoop;

// ---------------------------------------------------------------------------
// EventLoopPolicy — abstract base class defining the policy interface.
// ---------------------------------------------------------------------------

/// An abstract policy class that controls how EventLoops are created, accessed,
/// and managed. This is the C++ equivalent of Python's
/// `asyncio.AbstractEventLoopPolicy`.
///
/// Subclass this to provide custom event loop management strategies,
/// such as separate loops per thread, single-process loop reuse, or
/// platform-specific policies (e.g., Windows Proactor).
///
/// The default policy (`DefaultEventLoopPolicy`) maintains one event loop
/// per thread using thread-local storage.
class EventLoopPolicy {
 public:
  virtual ~EventLoopPolicy() = default;

  /// Returns the event loop for the current thread.
  ///
  /// If no event loop has been set for the current thread, this method
  /// creates one (by calling NewEventLoop()), sets it as the current loop,
  /// and returns it.
  ///
  /// This matches Python's `AbstractEventLoopPolicy.get_event_loop()`.
  /// \throws AsyncError if the current thread is not the main thread
  ///         and no loop has been set (Python's "no running event loop"
  ///         RuntimeError is not thrown here for simplicity).
  virtual EventLoop& GetEventLoop();

  /// Sets the event loop for the current thread to `loop`.
  ///
  /// This matches Python's `AbstractEventLoopPolicy.set_event_loop()`.
  virtual void SetEventLoop(EventLoop* loop);

  /// Creates a new event loop.
  ///
  /// The caller owns the returned pointer. It is the caller's responsibility
  /// to manage the loop's lifetime (typically by calling Close() or letting
  /// a Runner manage it).
  ///
  /// This matches Python's `AbstractEventLoopPolicy.new_event_loop()`.
  virtual EventLoop* NewEventLoop() = 0;

  /// Returns the event loop that is currently running on this thread,
  /// or nullptr if no loop is running.
  ///
  /// Unlike GetEventLoop(), this does NOT create a loop — it only returns
  /// the loop that was set as running by RunForever().
  ///
  /// This matches Python's `AbstractEventLoopPolicy.get_running_loop()`.
  virtual EventLoop* GetRunningLoop() const;

  /// Sets the "running" loop for the current thread.
  /// Used internally by EventLoop::RunForever() to mark a loop as running.
  /// This is also exposed publicly for advanced use cases.
  static void SetRunningLoop(EventLoop* loop);
};

// ---------------------------------------------------------------------------
// DefaultEventLoopPolicy — one loop per thread via thread_local storage.
// ---------------------------------------------------------------------------

/// The default event loop policy.
///
/// Each thread gets its own EventLoop instance, created lazily on first
/// access to GetEventLoop(). The loop is stored in thread-local storage
/// and persists until explicitly closed or the thread exits.
///
/// This matches Python's `asyncio.DefaultEventLoopPolicy`.
class DefaultEventLoopPolicy : public EventLoopPolicy {
 public:
  DefaultEventLoopPolicy() = default;

  /// Returns the event loop for the current thread, creating one if needed.
  EventLoop& GetEventLoop() override;

  /// Sets the event loop for the current thread.
  void SetEventLoop(EventLoop* loop) override;

  /// Creates a new EventLoop instance.
  EventLoop* NewEventLoop() override;

  /// Returns the running loop for the current thread.
  EventLoop* GetRunningLoop() const override;

 private:
  /// Returns the thread-local storage slot for the current thread's loop.
  static EventLoop*& ThreadLocalLoop();

  /// Returns the thread-local storage slot for the running loop.
  static EventLoop*& ThreadLocalRunningLoop();
};

// ---------------------------------------------------------------------------
// Global policy management.
// ---------------------------------------------------------------------------

/// Returns the current global EventLoopPolicy.
///
/// The global policy is initially a DefaultEventLoopPolicy instance.
/// This matches Python's `asyncio.get_event_loop_policy()`.
EventLoopPolicy& GetEventLoopPolicy();

/// Sets the global EventLoopPolicy.
///
/// The policy object must outlive all event loops created by it.
/// This matches Python's `asyncio.set_event_loop_policy()`.
void SetEventLoopPolicy(std::unique_ptr<EventLoopPolicy> policy);

/// Convenience function: returns the event loop for the current thread.
///
/// Creates a new loop if none exists for the current thread.
/// Equivalent to `GetEventLoopPolicy().GetEventLoop()`.
EventLoop& GetEventLoop();

/// Convenience function: sets the event loop for the current thread.
///
/// Equivalent to `GetEventLoopPolicy().SetEventLoop(loop)`.
void SetEventLoop(EventLoop* loop);

/// Convenience function: creates a new event loop.
///
/// The caller owns the returned pointer.
/// Equivalent to `GetEventLoopPolicy().NewEventLoop()`.
EventLoop* NewEventLoop();

/// Convenience function: returns the currently running event loop.
///
/// Returns nullptr if no loop is running on this thread.
/// Equivalent to `GetEventLoopPolicy().GetRunningLoop()`.
EventLoop* GetRunningLoop();

}  // namespace asyncio

#endif  // ASYNCIO_POLICY_H_
