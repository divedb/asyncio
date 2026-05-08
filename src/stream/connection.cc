// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Connection factory: OpenConnection / StartServer.

#include "asyncio/stream/connection.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include "asyncio/event_loop.h"
#include "asyncio/stream/stream_reader.h"
#include "asyncio/stream/stream_writer.h"
#include "asyncio/transport/protocol.h"
#include "asyncio/transport/transport.h"

namespace asyncio {

namespace detail {

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

void Server::SetPort(int port) { port_ = port; }

// ---------------------------------------------------------------------------
// ServerImpl
// ---------------------------------------------------------------------------

class ServerImpl : public Server {
 public:
  ServerImpl(EventLoop& loop,
             int listen_fd,
             std::function<void(asyncio::StreamReader&, asyncio::StreamWriter&)> client_handler);

  ~ServerImpl();

  Future<void> Close() override;
  Future<void> WaitClosed() override;
  int Port() const override { return port_; }
  size_t ConnectionCount() const override;
  void CloseAllConnections() override;

 private:
  void AcceptCallback();

  EventLoop* loop_ = nullptr;
  int listen_fd_ = -1;
  std::function<void(asyncio::StreamReader&, asyncio::StreamWriter&)> client_handler_;
  std::vector<std::shared_ptr<SocketTransport>> connections_;
  int port_ = 0;
};

ServerImpl::ServerImpl(
    EventLoop& loop,
    int listen_fd,
    std::function<void(asyncio::StreamReader&, asyncio::StreamWriter&)> client_handler)
    : loop_(&loop),
      listen_fd_(listen_fd),
      client_handler_(std::move(client_handler)) {
  // Register the accept callback.
  loop_->AddReader(listen_fd_, [this]() { AcceptCallback(); });
}

ServerImpl::~ServerImpl() {
  if (listen_fd_ >= 0) {
    loop_->RemoveReader(listen_fd_);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void ServerImpl::AcceptCallback() {
  struct sockaddr_storage client_addr;
  socklen_t addrlen = sizeof(client_addr);

  int client_fd = ::accept(
      listen_fd_,
      reinterpret_cast<struct sockaddr*>(&client_addr),
      &addrlen);
  if (client_fd < 0) {
    // EAGAIN/EWOULDBLOCK means no more connections right now.
    return;
  }

  // Set non-blocking.
  int flags = fcntl(client_fd, F_GETFL, 0);
  if (flags == -1) flags = 0;
  fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

  // Create reader, protocol, transport, and writer for the client.
  auto* reader = new StreamReader(*loop_);
  auto* protocol = new StreamProtocol();

  // Create transport first (owns fd).
  auto transport = std::make_shared<SocketTransport>(
      *loop_, client_fd, *protocol, reader, nullptr);
  auto* writer = new StreamWriter(*transport);
  protocol->SetStreams(reader, writer);
  transport->SetProtocol(*protocol);
  protocol->ConnectionMade(*transport);

  // Keep the transport alive.
  connections_.push_back(transport);

  // Call the user's client handler.
  try {
    client_handler_(*reader, *writer);
  } catch (...) {
    transport->Abort();
  }
}

Future<void> ServerImpl::Close() {
  if (listen_fd_ >= 0) {
    loop_->RemoveReader(listen_fd_);
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  CloseAllConnections();
  Future<void> f;
  f.SetResult();
  return f;
}

Future<void> ServerImpl::WaitClosed() {
  Future<void> f;
  f.SetResult();
  return f;
}

size_t ServerImpl::ConnectionCount() const { return connections_.size(); }

void ServerImpl::CloseAllConnections() {
  for (auto& conn : connections_) {
    conn->Abort();
  }
  connections_.clear();
}

}  // namespace detail

// ---------------------------------------------------------------------------
// OpenConnection
// ---------------------------------------------------------------------------

namespace {

void SetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) flags = 0;
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/// Resolves a hostname:port to a sockaddr_storage.
bool ResolveAddress(const std::string& host,
                    int port,
                    struct sockaddr_storage* out_addr) {
  std::memset(out_addr, 0, sizeof(*out_addr));

  // Try IPv4.
  struct sockaddr_in* sin =
      reinterpret_cast<struct sockaddr_in*>(out_addr);
  sin->sin_family = AF_INET;
  sin->sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &sin->sin_addr) == 1) return true;

  // Try IPv6.
  struct sockaddr_in6* sin6 =
      reinterpret_cast<struct sockaddr_in6*>(out_addr);
  sin6->sin6_family = AF_INET6;
  sin6->sin6_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET6, host.c_str(), &sin6->sin6_addr) == 1) return true;

  // Fall back to getaddrinfo.
  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  std::string port_str = std::to_string(port);
  struct addrinfo* result = nullptr;
  int r = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
  if (r != 0 || !result) return false;
  std::memcpy(out_addr, result->ai_addr,
              std::min(result->ai_addrlen,
                       static_cast<socklen_t>(sizeof(*out_addr))));
  freeaddrinfo(result);
  return true;
}

}  // namespace

/// Called when the socket becomes writable after a non-blocking connect.
static void OnConnectWritable(
    int fd,
    EventLoop* loop,
    const std::string& host,
    int port,
    Future<std::tuple<StreamReader*, StreamWriter*>>* result) {
  // Verify the connection succeeded.
  int so_error = 0;
  socklen_t optlen = sizeof(so_error);
  getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen);

  if (so_error != 0) {
    ::close(fd);
    try {
      throw AsyncError("connect failed to " + host + ":" + std::to_string(port) +
                       ": " + std::strerror(so_error));
    } catch (...) {
      result->SetException(std::current_exception());
    }
    loop->RemoveWriter(fd);
    delete result;
    return;
  }

  // Connected successfully! Build reader/writer/protocol/transport.
  auto* reader = new StreamReader(*loop);
  auto* protocol = new StreamProtocol();
  auto transport = std::make_shared<detail::SocketTransport>(
      *loop, fd, *protocol, reader, nullptr);
  auto* writer = new StreamWriter(*transport);
  protocol->SetStreams(reader, writer);
  transport->SetProtocol(*protocol);
  protocol->ConnectionMade(*transport);

  result->SetResult(std::make_tuple(reader, writer));
  loop->RemoveWriter(fd);
  delete result;
}

Future<std::tuple<StreamReader*, StreamWriter*>> OpenConnection(
    const std::string& host,
    int port,
    EventLoop* loop) {
  auto result = std::make_unique<
      Future<std::tuple<StreamReader*, StreamWriter*>>>();

  if (!loop) loop = EventLoop::Current();
  if (!loop) {
    try {
      throw AsyncError("no event loop available");
    } catch (...) {
      result->SetException(std::current_exception());
    }
    return *result;
  }

  // 1. Create socket.
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    try {
      throw AsyncError(std::string("socket: ") + std::strerror(errno));
    } catch (...) {
      result->SetException(std::current_exception());
    }
    return *result;
  }
  SetNonBlocking(fd);

  // 2. Resolve address.
  struct sockaddr_storage addr;
  if (!ResolveAddress(host, port, &addr)) {
    ::close(fd);
    try {
      throw AsyncError("getaddrinfo failed for: " + host);
    } catch (...) {
      result->SetException(std::current_exception());
    }
    return *result;
  }

  // 3. Connect (non-blocking).
  int r = ::connect(
      fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

  if (r == 0) {
    // Connected synchronously (localhost).
    auto* reader = new StreamReader(*loop);
    auto* protocol = new StreamProtocol();
    auto transport = std::make_shared<detail::SocketTransport>(
        *loop, fd, *protocol, reader, nullptr);
    auto* writer = new StreamWriter(*transport);
    protocol->SetStreams(reader, writer);
    transport->SetProtocol(*protocol);
    protocol->ConnectionMade(*transport);
    result->SetResult(std::make_tuple(reader, writer));
    return *result;
  }

  if (errno == EINPROGRESS) {
    // In progress — wait for writable.
    result->SetResult(std::tuple<StreamReader*, StreamWriter*>{});
    loop->AddWriter(fd, [loop, fd, host, port, result = result.release()]() {
      OnConnectWritable(fd, loop, host, port, result);
    });
    return *result;
  }

  // Other error.
  ::close(fd);
  try {
    throw AsyncError(std::string("connect: ") + std::strerror(errno));
  } catch (...) {
    result->SetException(std::current_exception());
  }
  return *result;
}

// ---------------------------------------------------------------------------
// StartServer
// ---------------------------------------------------------------------------

Future<std::unique_ptr<detail::Server>> StartServer(
    std::function<void(StreamReader&, StreamWriter&)> client_handler,
    const std::string& host,
    int port,
    EventLoop* loop) {
  auto result = std::make_unique<
      Future<std::unique_ptr<detail::Server>>>();

  if (!loop) loop = EventLoop::Current();
  if (!loop) {
    try {
      throw AsyncError("no event loop available");
    } catch (...) {
      result->SetException(std::current_exception());
    }
    return *result;
  }

  // 1. Create socket.
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    try {
      throw AsyncError(std::string("socket: ") + std::strerror(errno));
    } catch (...) {
      result->SetException(std::current_exception());
    }
    return *result;
  }

  // SO_REUSEADDR.
  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  SetNonBlocking(fd);

  // 2. Bind.
  struct sockaddr_in sin{};
  sin.sin_family = AF_INET;
  sin.sin_port = htons(static_cast<uint16_t>(port));
  if (host.empty() || host == "localhost" || host == "0.0.0.0") {
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (inet_pton(AF_INET, host.c_str(), &sin.sin_addr) != 1) {
    ::close(fd);
    try {
      throw AsyncError("invalid host: " + host);
    } catch (...) {
      result->SetException(std::current_exception());
    }
    return *result;
  }

  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin)) < 0) {
    ::close(fd);
    try {
      throw AsyncError(std::string("bind: ") + std::strerror(errno));
    } catch (...) {
      result->SetException(std::current_exception());
    }
    return *result;
  }

  // 3. Listen.
  if (::listen(fd, 128) < 0) {
    ::close(fd);
    try {
      throw AsyncError(std::string("listen: ") + std::strerror(errno));
    } catch (...) {
      result->SetException(std::current_exception());
    }
    return *result;
  }

  // 4. Get actual port if requested (port 0 = pick any free port).
  int actual_port = port;
  if (port == 0) {
    struct sockaddr_storage local;
    socklen_t len = sizeof(local);
    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&local), &len) == 0) {
      auto* ls = reinterpret_cast<struct sockaddr_in*>(&local);
      actual_port = ntohs(ls->sin_port);
    }
  }

  // 5. Create the server.
  auto server = std::make_unique<detail::ServerImpl>(
      *loop, fd, std::move(client_handler));
  server->SetPort(actual_port);

  result->SetResult(std::move(server));
  return *result;
}

}  // namespace asyncio
