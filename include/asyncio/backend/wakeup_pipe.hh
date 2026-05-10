#pragma once

#include "asyncio/backend/selector.hh"

namespace asyncio {

/// \brief Cross-platform wakeup primitive used to interrupt a blocking selector.
///
/// WakeupPipe implements the classic "self-pipe trick" in a portable form.
/// Internally it owns a connected pair of non-blocking sockets:
///
/// - The read endpoint is registered with the selector.
/// - The write endpoint is used to signal wakeups from other threads.
///
/// Calling Wakeup() writes a single byte to the write endpoint, causing the
/// read endpoint to become readable and immediately waking a blocking
/// `select()`, `poll()`, `epoll_wait()`, or `kevent()` call. After the
/// selector reports the read endpoint as ready, Drain() must be called to
/// consume all pending bytes and clear the wakeup condition.
///
/// Although the name refers to the traditional self-pipe technique, the
/// implementation uses socket pairs on all platforms:
///
/// - POSIX systems use `socketpair(AF_UNIX, SOCK_STREAM, 0, ...)`.
/// - Windows creates an equivalent connected pair using a loopback TCP
///   connection, since Winsock `select()` can only wait on socket handles.
///
/// The wakeup mechanism is level-triggered: multiple calls to Wakeup() may
/// coalesce into a single readiness notification until Drain() is invoked.
///
/// Lifetime and ownership:
///
/// - Objects are created exclusively by MakeWakeupPipe().
/// - The constructor is private to ensure platform-specific initialization is
///   centralized in the factory.
/// - The destructor closes both endpoints.
///
/// Thread safety:
///
/// - Wakeup() may be called concurrently from multiple threads.
/// - Drain() should only be called by the event loop thread.
/// - ReadHandle() is intended to be registered exactly once with a selector.
class WakeupPipe {
 public:
  /// \brief Constructs a new WakeupPipe instance with both endpoints initialized and ready for use.
  WakeupPipe();

  /// \brief Destroys the WakeupPipe instance, closing both the read and write endpoints.
  ~WakeupPipe();

  /// \brief Returns the read endpoint to register with the selector.
  ///
  /// \return The native handle for the read endpoint, which becomes readable when Wakeup() is
  ///         called.
  [[nodiscard]] NativeHandle ReadHandle() const noexcept { return read_handle_; }

  /// \brief Signals the wakeup endpoint, causing ReadHandle() to become readable.
  ///
  /// This method is thread-safe and may be called from any thread, including concurrently with
  /// itself. It is safe to call Wakeup() multiple times before Drain(); the selector will become
  /// readable after the first call, and subsequent calls will have no additional effect until
  /// Drain() is called to clear the wakeup condition.
  ///
  /// \throws std::system_error if the underlying write operation fails with an error other than
  ///         EAGAIN / WSAEWOULDBLOCK, which indicates that the wakeup byte could not be queued. In
  ///         that case, the selector may not be woken as expected.
  void Wakeup();

  /// \brief Reads and discards all pending wakeup bytes.
  ///
  /// This should be called after the selector reports ReadHandle() as
  /// readable.
  void Drain();

 private:
  /// Endpoint monitored by the selector for readability.
  NativeHandle read_handle_{kInvalidHandle};

  /// Endpoint used to write wakeup notifications.
  NativeHandle write_handle_{kInvalidHandle};
};

}  // namespace asyncio