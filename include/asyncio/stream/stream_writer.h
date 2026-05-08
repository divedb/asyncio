// Copyright 2025 asyncio-cpp authors. All rights reserved.
// StreamWriter — buffered async byte stream writer.
// Mirrors Python's asyncio.StreamWriter.

#ifndef ASYNCIO_STREAM_WRITER_H_
#define ASYNCIO_STREAM_WRITER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "asyncio/future.h"

namespace asyncio {

class EventLoop;
class TransportBase;

namespace detail {
class SocketTransport;
}  // namespace detail

// ---------------------------------------------------------------------------
// StreamWriter
// ---------------------------------------------------------------------------

/// Buffered async writer for byte streams.
///
/// StreamWriter wraps a lower-level transport (typically a SocketTransport)
/// and exposes high-level write methods:
///
///   void Write(data)            — buffer data for sending
///   Future<void> Drain()       — wait until the write buffer is flushed
///   void Close()               — close the connection
///   Future<void> WaitClosed()  — wait until fully closed
///
/// The write buffer is drained automatically on the event loop thread.
///
/// Thread safety: all public methods must be called from the event loop thread.
class StreamWriter {
 public:
  /// Constructs a StreamWriter.
  explicit StreamWriter(detail::SocketTransport& transport);

  ~StreamWriter();

  // Non-copyable.
  StreamWriter(const StreamWriter&) = delete;
  StreamWriter& operator=(const StreamWriter&) = delete;

  // --- Write methods ---

  /// Buffers data for sending. Non-blocking.
  /// The data is copied into the internal buffer.
  void Write(std::span<const uint8_t> data);

  /// Writes a string (convenience overload).
  void Write(std::string_view data);

  /// Writes multiple buffers in sequence.
  void Writelines(std::span<std::span<const uint8_t>> data);

  /// Returns a Future that resolves when the write buffer is empty and the
  /// transport has finished sending all buffered data.
  /// This is the backpressure control point.
  Future<void> Drain();

  // --- Half-close ---

  /// Sends a TCP half-close (FIN). The peer will receive EOF on its read side.
  /// The local side can still write data until WriteEof() or Close().
  void WriteEof();

  /// Returns true (TCP sockets support half-close).
  [[nodiscard]] bool CanWriteEof() const { return true; }

  // --- Lifecycle ---

  /// Closes the transport and waits for the connection to fully close.
  /// Equivalent to calling `Close()` then `co_await WaitClosed()`.
  Future<void> Close();

  /// Returns true if the transport is closing or has been closed.
  [[nodiscard]] bool IsClosing() const;

  /// Waits until the transport is fully closed.
  Future<void> WaitClosed();

  // --- Meta ---

  /// Returns transport extra info (peername, sockname, socket).
  [[nodiscard]] std::string GetExtraInfo(const std::string& name) const;

 private:
  detail::SocketTransport& transport_;
  bool closed_ = false;
};

}  // namespace asyncio

#endif  // ASYNCIO_STREAM_WRITER_H_
