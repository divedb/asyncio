// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Protocol interfaces for async I/O.

#ifndef ASYNCIO_TRANSPORT_PROTOCOL_H_
#define ASYNCIO_TRANSPORT_PROTOCOL_H_

#include <cstddef>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace asyncio {

class TransportBase;

// ---------------------------------------------------------------------------
// ProtocolBase
// ---------------------------------------------------------------------------

/// Base class for all protocol implementations.
///
/// Mirrors Python's `asyncio.BaseProtocol`.
class ProtocolBase {
 public:
  virtual ~ProtocolBase() = default;

  /// Called when the transport is connected and ready.
  virtual void ConnectionMade(TransportBase& transport) = 0;

  /// Called when the transport receives data from the peer.
  virtual void DataReceived(std::span<const uint8_t> data) = 0;

  /// Called when the transport's connection is closed.
  /// @param ex  nullptr if closed cleanly; otherwise the error.
  virtual void ConnectionLost(std::exception_ptr ex) = 0;

  /// Called when the transport's write buffer is full.
  virtual void PauseWriting() {}

  /// Called when the transport's write buffer has drained.
  virtual void ResumeWriting() {}
};

// ---------------------------------------------------------------------------
// StreamProtocol
// ---------------------------------------------------------------------------

/// Concrete protocol that bridges between a SocketTransport and
/// StreamReader/StreamWriter.
class StreamProtocol : public ProtocolBase {
 public:
  void SetStreams(class StreamReader* reader, class StreamWriter* writer);

  // ProtocolBase interface.
  void ConnectionMade(TransportBase& transport) override;
  void DataReceived(std::span<const uint8_t> data) override;
  void ConnectionLost(std::exception_ptr ex) override;
  void PauseWriting() override;
  void ResumeWriting() override;

 private:
  class StreamReader* reader_ = nullptr;
  class StreamWriter* writer_ = nullptr;
  TransportBase* transport_ = nullptr;
};

}  // namespace asyncio

#endif  // ASYNCIO_TRANSPORT_PROTOCOL_H_
