// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Connection factory — OpenConnection / StartServer.
// Mirrors Python's asyncio.streams module.

#ifndef ASYNCIO_STREAM_CONNECTION_H_
#define ASYNCIO_STREAM_CONNECTION_H_

#include <functional>
#include <memory>
#include <string>
#include <tuple>

#include "asyncio/future.h"

namespace asyncio {

class EventLoop;
class StreamReader;
class StreamWriter;

namespace detail {
class SocketTransport;
class Server;
}  // namespace detail

// ---------------------------------------------------------------------------
// OpenConnection
// ---------------------------------------------------------------------------

/// Opens a TCP client connection asynchronously.
///
/// Mirrors `asyncio.open_connection()`.
///
/// Usage:
///   auto [reader, writer] = co_await OpenConnection("example.com", 80);
///
/// @param host  Hostname or IP address.
/// @param port  TCP port number.
/// @param loop  Event loop to use (defaults to the current loop).
/// @return A tuple of (StreamReader, StreamWriter).
Future<std::tuple<StreamReader*, StreamWriter*>> OpenConnection(
    const std::string& host,
    int port,
    EventLoop* loop = nullptr);

// ---------------------------------------------------------------------------
// StartServer
// ---------------------------------------------------------------------------

/// Starts a TCP server that accepts incoming connections.
///
/// Mirrors `asyncio.start_server()`.
///
/// Usage:
///   auto server = co_await StartServer(
///       [](StreamReader& r, StreamWriter& w) {
///         // handle client connection
///       },
///       "localhost", 8080);
///
/// @param client_handler  Callback invoked for each incoming connection.
///                        Receives (StreamReader&, StreamWriter&).
/// @param host            Address to bind to ("" for all interfaces).
/// @param port            TCP port number (0 = choose any free port).
/// @param loop            Event loop to use (defaults to the current loop).
/// @return A Server object; call server.Close() to stop.
Future<std::unique_ptr<detail::Server>> StartServer(
    std::function<void(StreamReader&, StreamWriter&)> client_handler,
    const std::string& host,
    int port,
    EventLoop* loop = nullptr);

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

namespace detail {

/// Manages a listening server socket created by StartServer().
/// Mirrors Python's `asyncio.Server`.
class Server {
 public:
  virtual ~Server() = default;

  /// Closes the server socket and stops accepting new connections.
  virtual Future<void> Close() = 0;

  /// Waits until the server is fully closed.
  virtual Future<void> WaitClosed() = 0;

  /// Returns the port the server is listening on (0 if not yet bound).
  [[nodiscard]] virtual int Port() const = 0;

  /// Returns the number of active client connections.
  [[nodiscard]] virtual size_t ConnectionCount() const = 0;

  /// Closes all active client connections immediately.
  virtual void CloseAllConnections() = 0;

  /// Sets the port number (used by the implementation).
  void SetPort(int port);

 private:
  int port_ = 0;
};

}  // namespace detail
}  // namespace asyncio

#endif  // ASYNCIO_STREAM_CONNECTION_H_
