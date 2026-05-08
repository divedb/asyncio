// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Selector — abstract interface for I/O multiplexing backends.

#ifndef ASYNCIO_DETAIL_SELECTOR_H_
#define ASYNCIO_DETAIL_SELECTOR_H_

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace asyncio::detail {

/// Bitmask of I/O event types that can be monitored.
enum IoEventFlags : uint32_t {
  kReadable = 1u << 0,  ///< File descriptor is ready for reading.
  kWritable = 1u << 1,  ///< File descriptor is ready for writing.
};

/// Describes a single I/O readiness event returned by Selector::Select().
struct IoEvent {
  int fd;        ///< The file descriptor that became ready.
  uint32_t events;  ///< Bitmask of IoEventFlags indicating what is ready.

  /// True if the fd is ready for reading.
  [[nodiscard]] bool readable() const { return (events & kReadable) != 0; }

  /// True if the fd is ready for writing.
  [[nodiscard]] bool writable() const { return (events & kWritable) != 0; }
};

/// Abstract I/O multiplexing interface.
///
/// Implementations: KqueueSelector (macOS/BSD), EpollSelector (Linux),
/// SelectSelector (portable fallback).
///
/// Thread safety: NOT thread-safe. All methods must be called from the
/// event loop thread.
class Selector {
 public:
  virtual ~Selector() = default;

  /// Registers fd for monitoring.
  ///
  /// @param fd     File descriptor to monitor.
  /// @param events Bitmask of IoEventFlags (kReadable | kWritable).
  /// @throws std::system_error on failure.
  virtual void Register(int fd, uint32_t events) = 0;

  /// Modifies the event mask for an already-registered fd.
  ///
  /// @param fd     File descriptor already registered.
  /// @param events New bitmask of IoEventFlags.
  /// @throws std::system_error on failure.
  virtual void Modify(int fd, uint32_t events) = 0;

  /// Removes fd from monitoring.
  ///
  /// No-op if fd was not registered.
  /// @param fd File descriptor to unregister.
  virtual void Unregister(int fd) = 0;

  /// Waits for I/O events, returning ready file descriptors.
  ///
  /// @param timeout Maximum time to block. nullopt means block indefinitely.
  ///                Zero means poll (return immediately without blocking).
  /// @return List of IoEvent, one per ready fd (may have multiple events per
  ///         fd if both readable and writable).
  virtual std::vector<IoEvent> Select(
      std::optional<std::chrono::nanoseconds> timeout) = 0;
};

}  // namespace asyncio::detail

#endif  // ASYNCIO_DETAIL_SELECTOR_H_
