// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Transport — raw socket I/O owner with non-blocking semantics.

#ifndef ASYNCIO_TRANSPORT_TRANSPORT_H_
#define ASYNCIO_TRANSPORT_TRANSPORT_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "asyncio/error.h"
#include "asyncio/future.h"

// For sockaddr_storage on POSIX systems.
#if defined(__unix__) || defined(__APPLE__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace asyncio {

class EventLoop;
class ProtocolBase;
class StreamReader;
class StreamWriter;

// ---------------------------------------------------------------------------
// TransportBase
// ---------------------------------------------------------------------------

/// Abstract base class for all transports.
///
/// A transport is responsible for reading and writing bytes on behalf of
/// a protocol. This mirrors Python's `asyncio.BaseTransport`.
class TransportBase {
 public:
  virtual ~TransportBase() = default;

  /// Returns true if the transport is closing or has been closed.
  [[nodiscard]] virtual bool IsClosing() const = 0;

  /// Closes the transport without flushing the write buffer.
  virtual void Abort() = 0;

  /// Gets transport meta-information.
  /// Supported names: "peername", "sockname", "socket".
  [[nodiscard]] virtual std::string GetExtraInfo(const std::string& name) const = 0;
};

namespace detail {

// ---------------------------------------------------------------------------
// SocketTransport
// ---------------------------------------------------------------------------

/// Owns a non-blocking TCP socket and performs async I/O through the
/// event loop's I/O multiplexing layer.
class SocketTransport : public asyncio::TransportBase,
                       public std::enable_shared_from_this<SocketTransport> {
 public:
  /// Creates a new SocketTransport wrapping an already-connected socket fd.
  /// Takes ownership of fd.
  SocketTransport(EventLoop& loop,
                  int fd,
                  asyncio::ProtocolBase& protocol,
                  StreamReader* reader,
                  StreamWriter* writer);

  ~SocketTransport();

  // Non-copyable, non-movable.
  SocketTransport(const SocketTransport&) = delete;
  SocketTransport& operator=(const SocketTransport&) = delete;
  SocketTransport(SocketTransport&&) = delete;
  SocketTransport& operator=(SocketTransport&&) = delete;

  // --- TransportBase ---
  bool IsClosing() const override { return closing_; }
  void Abort() override;
  std::string GetExtraInfo(const std::string& name) const override;

  // --- Protocol management ---
  void SetProtocol(asyncio::ProtocolBase& protocol);
  asyncio::ProtocolBase* GetProtocol() const { return protocol_; }

  // --- ReadTransportBase ---
  void PauseReading();
  void ResumeReading();

  // --- WriteTransportBase ---
  void Write(std::span<const uint8_t> data);
  void Writelines(std::span<std::span<const uint8_t>> data);
  void WriteEof();
  [[nodiscard]] bool CanWriteEof() const { return true; }
  Future<void> Drain();
  Future<void> Close();
  Future<void> WaitClosed();

  // --- I/O helpers ---
  Future<std::vector<uint8_t>> Read(size_t n);
  Future<void> WriteAll(std::span<const uint8_t> data);

  // --- Server factory ---
  static std::shared_ptr<SocketTransport> Accept(
      EventLoop& loop,
      int server_fd,
      ProtocolBase& protocol,
      StreamReader* reader,
      StreamWriter* writer);

 private:
  void OnSocketReadable();
  void OnSocketWritable();
  void PerformWrite();

  EventLoop* loop_ = nullptr;
  int fd_ = -1;
  asyncio::ProtocolBase* protocol_ = nullptr;
  StreamReader* reader_ = nullptr;

  // Reading.
  bool reading_paused_ = false;
  Future<std::vector<uint8_t>>* read_future_ = nullptr;

  // Writing.
  std::vector<uint8_t> write_buffer_;
  bool writing_ = false;
  Future<void>* write_future_ = nullptr;

  // Closing.
  bool closing_ = false;
  bool eof_written_ = false;
  Future<void> close_future_;
  std::shared_ptr<Future<void>> wait_closed_future_;
  std::shared_ptr<Future<void>> drain_future_;
  int close_stage_ = 0;  // 0=open, 1=close initiated, 2=write done, 3=done
};

// ---------------------------------------------------------------------------
// SocketAddress
// ---------------------------------------------------------------------------

class SocketAddress {
 public:
  SocketAddress();
  explicit SocketAddress(const struct sockaddr_storage& addr);
  std::string ToString() const;
  bool IsValid() const;

 private:
  struct sockaddr_storage addr_;
};

}  // namespace detail
}  // namespace asyncio

#endif  // ASYNCIO_TRANSPORT_TRANSPORT_H_
