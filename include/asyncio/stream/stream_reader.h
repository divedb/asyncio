// Copyright 2025 asyncio-cpp authors. All rights reserved.
// StreamReader — buffered async byte stream reader.
// Mirrors Python's asyncio.StreamReader.

#ifndef ASYNCIO_STREAM_READER_H_
#define ASYNCIO_STREAM_READER_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "asyncio/error.h"
#include "asyncio/future.h"

namespace asyncio {

// Forward declaration.
class EventLoop;

// ---------------------------------------------------------------------------
// StreamReader
// ---------------------------------------------------------------------------

/// Buffered async reader for byte streams.
///
/// StreamReader wraps a lower-level transport (typically a SocketTransport)
/// and exposes high-level read methods that return Futures:
///
///   Future<std::vector<uint8_t>> Read(n)        — up to n bytes
///   Future<std::vector<uint8_t>> ReadExactly(n) — exactly n bytes
///   Future<std::vector<uint8_t>> ReadUntil(sep) — until separator
///   Future<std::string>         ReadLine()      — until '\n'
///
/// Data flows from the transport into the internal buffer via `FeedData()`,
/// which is called by the protocol's `DataReceived()` callback.
///
/// Backpressure: when the internal buffer exceeds 2× the limit (default 64 KiB),
/// `MaybePause()` returns true and the caller should call `Pause()` to stop
/// receiving more data. `Resume()` restarts reception.
///
/// Thread safety: all public methods must be called from the event loop thread.
class StreamReader {
 public:
  /// Default high-water mark for backpressure (64 KiB).
  static constexpr size_t kDefaultLimit = 64 * 1024;

  /// Constructs a StreamReader.
  explicit StreamReader(EventLoop& loop);

  ~StreamReader();

  // Non-copyable.
  StreamReader(const StreamReader&) = delete;
  StreamReader& operator=(const StreamReader&) = delete;

  // --- High-level read methods ---

  /// Reads up to |n| bytes from the stream.
  /// Returns immediately if data is buffered; otherwise waits.
  /// Returns an empty vector on EOF.
  Future<std::vector<uint8_t>> Read(size_t n);

  /// Reads exactly |n| bytes.
  /// Raises IncompleteReadError if EOF is hit before n bytes are collected.
  Future<std::vector<uint8_t>> ReadExactly(size_t n);

  /// Reads until the first occurrence of |separator|.
  /// The separator is included in the returned data.
  /// Raises LimitOverrunError if the buffer exceeds |limit_| bytes without
  /// finding the separator.
  Future<std::vector<uint8_t>> ReadUntil(std::string_view separator);

  /// Reads a line (until '\n'), returning the line without the '\n'.
  /// Shortcut for `ReadUntil("\n")` with a newline stripped.
  Future<std::string> ReadLine();

  // --- State queries ---

  /// Returns true if EOF has been received.
  [[nodiscard]] bool AtEof() const { return eof_; }

  /// Returns true if there is buffered data available.
  [[nodiscard]] bool IsReadable() const { return !buffer_.empty(); }

  /// Returns the current size of the internal buffer.
  [[nodiscard]] size_t BufferSize() const { return buffer_.size(); }

  // --- Protocol / transport interface ---

  /// Injects data received from the transport into the buffer.
  /// Wakes up any pending read coroutines.
  void FeedData(std::span<const uint8_t> data);

  /// Injects a zero-length chunk to signal EOF.
  /// No more data will arrive after this call.
  void FeedEof();

  /// Injects an error into the reader. All pending reads are resolved with
  /// this exception.
  void SetException(std::exception_ptr ex);

  // --- Backpressure control ---

  /// Sets the buffer size limit for backpressure (default: kDefaultLimit).
  void SetLimit(size_t limit);

  /// Returns true if the buffer size exceeds 2× the limit, indicating
  /// the caller should pause reading.
  [[nodiscard]] bool MaybePause() const;

  /// Pauses the reader. Called by the transport when MaybePause() is true.
  void Pause();

  /// Resumes the reader. Called by the transport after data is consumed.
  void Resume();

  /// Returns true if reading is currently paused.
  [[nodiscard]] bool IsPaused() const { return paused_; }

  // --- Internal: called by the protocol to get callbacks ---
  /// Returns the DataReceived callback for the protocol.
  std::function<void(std::span<const uint8_t>)> DataReceivedCallback();

 private:
  // Resolves the first pending read with available data.
  void ResolvePendingReads();

  // Searches for a separator in the buffer.
  // Returns the byte index of the separator, or nullopt if not found.
  std::optional<size_t> FindSeparator(std::string_view sep) const;

  // --- State ---
  EventLoop* loop_;
  std::deque<uint8_t> buffer_;  // Accumulated incoming bytes.

  // Pending read operations (in order).
  struct ReadRequest {
    size_t n;  // For Read(n)
    bool exact;  // For ReadExactly
    std::string_view separator;  // For ReadUntil/ReadLine
    Future<std::vector<uint8_t>> future;
  };
  std::deque<ReadRequest> pending_reads_;

  // Special read state for ReadExactly / ReadUntil / ReadLine.
  // When a multi-step read is in progress, these hold the in-flight state.
  std::optional<ReadRequest> multi_read_;  // Active multi-step read.

  // Exception state.
  std::exception_ptr exception_;
  bool eof_ = false;

  // Backpressure.
  size_t limit_ = kDefaultLimit;
  bool paused_ = false;

  // Callback from transport to resume reading.
  std::function<void()> resume_callback_;
};

}  // namespace asyncio

#endif  // ASYNCIO_STREAM_READER_H_
