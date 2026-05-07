// Copyright 2025 asyncio-cpp authors. All rights reserved.
// SelfPipe — cross-thread wakeup mechanism for the event loop.

#ifndef ASYNCIO_DETAIL_SELF_PIPE_H_
#define ASYNCIO_DETAIL_SELF_PIPE_H_

#include <cstddef>
#include <utility>

namespace asyncio::detail {

/// A cross-thread wakeup mechanism using a Unix socket pair.
///
/// The write end can be signaled from any thread (e.g., by
/// CallSoonThreadsafe). The read end is registered with the event loop's
/// I/O multiplexer so that the loop wakes up when signaled.
///
/// Platform optimizations:
///   - Linux: could use eventfd() instead (future work).
///   - macOS/BSD: could use kqueue EVFILT_USER (future work).
///   - Portable: socketpair() (current implementation).
class SelfPipe {
 public:
  /// Creates the socket pair. The read end is non-blocking.
  SelfPipe();

  /// Closes both ends of the socket pair.
  ~SelfPipe();

  /// Non-copyable, non-movable (owns file descriptors).
  SelfPipe(const SelfPipe&) = delete;
  SelfPipe& operator=(const SelfPipe&) = delete;
  SelfPipe(SelfPipe&&) = delete;
  SelfPipe& operator=(SelfPipe&&) = delete;

  /// Returns the file descriptor for the read end.
  /// Register this with the event loop's selector for read-ready events.
  [[nodiscard]] int ReadFd() const;

  /// Signals the pipe by writing a single byte. Thread-safe.
  void Wakeup();

  /// Reads and discards all pending bytes from the read end.
  /// Call this when the selector reports the read end as ready.
  void Drain();

 private:
  int read_fd_ = -1;
  int write_fd_ = -1;
};

}  // namespace asyncio::detail

#endif  // ASYNCIO_DETAIL_SELF_PIPE_H_
