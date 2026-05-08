// Copyright 2025 asyncio-cpp authors. All rights reserved.
// socket_ops.cc — Implementation of async socket operations.

#include "asyncio/socket/socket_ops.h"

#include <cstring>
#include <utility>

#include "asyncio/event_loop.h"

namespace asyncio {
namespace socket_ops {

// ============================================================================
// Helper
// ============================================================================

inline EventLoop* GetLoop(EventLoop* loop) {
  if (!loop) return EventLoop::Current();
  return loop;
}

// ============================================================================
// SockRecvInto
// ============================================================================

Future<size_t> SockRecvInto(
    std::shared_ptr<TcpSocket> sock,
    std::span<uint8_t> buf,
    EventLoop* loop) {
  loop = GetLoop(loop);
  if (!loop) {
    Future<size_t> fut;
    fut.SetException(std::make_exception_ptr(
        std::runtime_error("No event loop")));
    return fut;
  }

  ssize_t n = ::recv(sock->Fd(), reinterpret_cast<char*>(buf.data()), buf.size(), 0);
  if (n >= 0) {
    Future<size_t> fut;
    fut.SetResult(static_cast<size_t>(n));
    return fut;
  }

  int err = socket_os::GetLastError();
  if (err == EAGAIN || err == EWOULDBLOCK) {
    struct Context {
      std::shared_ptr<TcpSocket> sock;
      std::span<uint8_t> buf;
      Future<size_t> future;
    };

    auto* ctx = new Context{std::move(sock), buf, {}};

    loop->AddReader(ctx->sock->Fd(), [ctx, loop]() {
      loop->RemoveReader(ctx->sock->Fd());
      ssize_t n = ::recv(ctx->sock->Fd(),
                         reinterpret_cast<char*>(ctx->buf.data()),
                         ctx->buf.size(), 0);
      if (n >= 0) {
        ctx->future.SetResult(static_cast<size_t>(n));
      } else {
        ctx->future.SetException(std::make_exception_ptr(
            SocketException::FromLastError()));
      }
      delete ctx;
    });

    return ctx->future;
  }

  Future<size_t> fut;
  fut.SetException(std::make_exception_ptr(SocketException::FromErrno(err)));
  return fut;
}

// ============================================================================
// SockRecv
// ============================================================================

Future<std::vector<uint8_t>> SockRecv(
    std::shared_ptr<TcpSocket> sock,
    size_t max_len,
    EventLoop* loop) {
  loop = GetLoop(loop);
  if (!loop) {
    Future<std::vector<uint8_t>> fut;
    fut.SetException(std::make_exception_ptr(
        std::runtime_error("No event loop")));
    return fut;
  }

  std::vector<uint8_t> buf(max_len);
  ssize_t n = ::recv(sock->Fd(), reinterpret_cast<char*>(buf.data()), max_len, 0);
  if (n >= 0) {
    buf.resize(static_cast<size_t>(n));
    Future<std::vector<uint8_t>> fut;
    fut.SetResult(std::move(buf));
    return fut;
  }

  int err = socket_os::GetLastError();
  if (err == EAGAIN || err == EWOULDBLOCK) {
    struct Context {
      std::shared_ptr<TcpSocket> sock;
      std::vector<uint8_t> buf;
      size_t max_len;
      Future<std::vector<uint8_t>> future;
    };

    auto* ctx = new Context{std::move(sock), std::move(buf), max_len, {}};

    loop->AddReader(ctx->sock->Fd(), [ctx, loop]() {
      loop->RemoveReader(ctx->sock->Fd());
      ssize_t n = ::recv(ctx->sock->Fd(),
                         reinterpret_cast<char*>(ctx->buf.data()),
                         ctx->max_len, 0);
      if (n >= 0) {
        ctx->buf.resize(static_cast<size_t>(n));
        ctx->future.SetResult(std::move(ctx->buf));
      } else {
        ctx->future.SetException(std::make_exception_ptr(
            SocketException::FromLastError()));
      }
      delete ctx;
    });

    return ctx->future;
  }

  Future<std::vector<uint8_t>> fut;
  fut.SetException(std::make_exception_ptr(SocketException::FromErrno(err)));
  return fut;
}

// ============================================================================
// SockSendAll
// ============================================================================

namespace detail {

struct SendAllContext {
  std::shared_ptr<TcpSocket> sock;
  std::span<const uint8_t> data;
  size_t offset = 0;
  Future<size_t> future;
};

}  // namespace detail

static void ContinueSendAll(detail::SendAllContext* ctx, EventLoop* loop) {
  ssize_t n = ::send(ctx->sock->Fd(),
                     reinterpret_cast<const char*>(ctx->data.data() + ctx->offset),
                     ctx->data.size() - ctx->offset, 0);
  if (n < 0) {
    loop->RemoveWriter(ctx->sock->Fd());
    ctx->future.SetException(std::make_exception_ptr(
        SocketException::FromLastError()));
    delete ctx;
    return;
  }

  ctx->offset += static_cast<size_t>(n);
  if (ctx->offset >= ctx->data.size()) {
    loop->RemoveWriter(ctx->sock->Fd());
    ctx->future.SetResult(ctx->data.size());
    delete ctx;
    return;
  }

  loop->AddWriter(ctx->sock->Fd(), [ctx, loop]() {
    ContinueSendAll(ctx, loop);
  });
}

Future<size_t> SockSendAll(
    std::shared_ptr<TcpSocket> sock,
    std::span<const uint8_t> data,
    EventLoop* loop) {
  loop = GetLoop(loop);
  if (!loop) {
    Future<size_t> fut;
    fut.SetException(std::make_exception_ptr(
        std::runtime_error("No event loop")));
    return fut;
  }

  ssize_t n = ::send(sock->Fd(),
                     reinterpret_cast<const char*>(data.data()),
                     data.size(), 0);
  if (n == static_cast<ssize_t>(data.size())) {
    Future<size_t> fut;
    fut.SetResult(data.size());
    return fut;
  }
  if (n >= 0) {
    auto* ctx = new detail::SendAllContext{std::move(sock), data, static_cast<size_t>(n), {}};
    loop->AddWriter(ctx->sock->Fd(), [ctx, loop]() {
      ContinueSendAll(ctx, loop);
    });
    return ctx->future;
  }

  int err = socket_os::GetLastError();
  if (err == EAGAIN || err == EWOULDBLOCK) {
    auto* ctx = new detail::SendAllContext{std::move(sock), data, 0, {}};
    loop->AddWriter(ctx->sock->Fd(), [ctx, loop]() {
      ContinueSendAll(ctx, loop);
    });
    return ctx->future;
  }

  Future<size_t> fut;
  fut.SetException(std::make_exception_ptr(SocketException::FromErrno(err)));
  return fut;
}

// ============================================================================
// SockSend
// ============================================================================

Future<size_t> SockSend(
    std::shared_ptr<TcpSocket> sock,
    std::span<const uint8_t> data,
    EventLoop* loop) {
  loop = GetLoop(loop);
  if (!loop) {
    Future<size_t> fut;
    fut.SetException(std::make_exception_ptr(
        std::runtime_error("No event loop")));
    return fut;
  }

  ssize_t n = ::send(sock->Fd(),
                     reinterpret_cast<const char*>(data.data()),
                     data.size(), 0);
  if (n >= 0) {
    Future<size_t> fut;
    fut.SetResult(static_cast<size_t>(n));
    return fut;
  }

  int err = socket_os::GetLastError();
  if (err == EAGAIN || err == EWOULDBLOCK) {
    struct Context {
      std::shared_ptr<TcpSocket> sock;
      std::span<const uint8_t> data;
      Future<size_t> future;
    };

    auto* ctx = new Context{std::move(sock), data, {}};

    loop->AddWriter(ctx->sock->Fd(), [ctx, loop]() {
      loop->RemoveWriter(ctx->sock->Fd());
      ssize_t n = ::send(ctx->sock->Fd(),
                         reinterpret_cast<const char*>(ctx->data.data()),
                         ctx->data.size(), 0);
      if (n >= 0) {
        ctx->future.SetResult(static_cast<size_t>(n));
      } else {
        ctx->future.SetException(std::make_exception_ptr(
            SocketException::FromLastError()));
      }
      delete ctx;
    });

    return ctx->future;
  }

  Future<size_t> fut;
  fut.SetException(std::make_exception_ptr(SocketException::FromErrno(err)));
  return fut;
}

// ============================================================================
// SockConnect
// ============================================================================

namespace detail {

struct ConnectContext {
  std::shared_ptr<TcpSocket> sock;
  Future<void> future;
};

}  // namespace detail

Future<void> SockConnect(
    std::shared_ptr<TcpSocket> sock,
    const SockAddr& addr,
    EventLoop* loop) {
  loop = GetLoop(loop);
  if (!loop) {
    Future<void> fut;
    fut.SetException(std::make_exception_ptr(
        std::runtime_error("No event loop")));
    return fut;
  }

  sock->SetNonBlocking(true);

  int err = ::connect(sock->Fd(), addr.CAddr(), addr.Len());
  if (err == 0) {
    Future<void> fut;
    fut.SetResult();
    return fut;
  }

  err = socket_os::GetLastError();
#ifdef ASYNCIO_OS_WINDOWS
  if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
#else
  if (err == EINPROGRESS) {
#endif
    auto* ctx = new detail::ConnectContext{std::move(sock), {}};

    loop->AddWriter(ctx->sock->Fd(), [ctx, loop]() {
      loop->RemoveWriter(ctx->sock->Fd());
      int err = socket_os::GetSocketError(ctx->sock->Fd());
      if (err == 0) {
        ctx->future.SetResult();
      } else {
        ctx->future.SetException(std::make_exception_ptr(
            SocketException::FromErrno(err)));
      }
      delete ctx;
    });

    return ctx->future;
  }

  Future<void> fut;
  fut.SetException(std::make_exception_ptr(SocketException::FromErrno(err)));
  return fut;
}

Future<void> SockConnect(
    std::shared_ptr<TcpSocket> sock,
    const std::string& host,
    int port,
    EventLoop* loop) {
  auto addr = SockAddr::CreateIPv4(host, port);
  return SockConnect(std::move(sock), addr, loop);
}

// ============================================================================
// SockAccept
// ============================================================================

Future<std::tuple<std::shared_ptr<TcpSocket>, SockAddr>> SockAccept(
    std::shared_ptr<TcpListener> listener,
    EventLoop* loop) {
  loop = GetLoop(loop);
  if (!loop) {
    Future<std::tuple<std::shared_ptr<TcpSocket>, SockAddr>> fut;
    fut.SetException(std::make_exception_ptr(
        std::runtime_error("No event loop")));
    return fut;
  }

  sockaddr_storage addr_storage{};
  socklen_t addr_len = sizeof(addr_storage);

  SocketFd client_fd = ::accept(listener->Fd(),
                                 reinterpret_cast<sockaddr*>(&addr_storage),
                                 &addr_len);

  if (client_fd != kInvalidSocket) {
    SockAddr addr(reinterpret_cast<sockaddr*>(&addr_storage), addr_len);
    auto client = TcpSocket::CreateFromFd(client_fd);
    client->SetNonBlocking(true);
    Future<std::tuple<std::shared_ptr<TcpSocket>, SockAddr>> fut;
    fut.SetResult(std::make_tuple(std::move(client), std::move(addr)));
    return fut;
  }

  int err = socket_os::GetLastError();
  if (err == EAGAIN || err == EWOULDBLOCK) {
    struct Context {
      std::shared_ptr<TcpListener> listener;
      Future<std::tuple<std::shared_ptr<TcpSocket>, SockAddr>> future;
    };

    auto* ctx = new Context{std::move(listener), {}};

    loop->AddReader(ctx->listener->Fd(), [ctx, loop]() {
      loop->RemoveReader(ctx->listener->Fd());

      sockaddr_storage addr_storage{};
      socklen_t addr_len = sizeof(addr_storage);

      SocketFd client_fd = ::accept(ctx->listener->Fd(),
                                    reinterpret_cast<sockaddr*>(&addr_storage),
                                    &addr_len);

      if (client_fd != kInvalidSocket) {
        SockAddr addr(reinterpret_cast<sockaddr*>(&addr_storage), addr_len);
        auto client = TcpSocket::CreateFromFd(client_fd);
        client->SetNonBlocking(true);
        ctx->future.SetResult(std::make_tuple(std::move(client), std::move(addr)));
      } else {
        ctx->future.SetException(std::make_exception_ptr(
            SocketException::FromLastError()));
      }
      delete ctx;
    });

    return ctx->future;
  }

  Future<std::tuple<std::shared_ptr<TcpSocket>, SockAddr>> fut;
  fut.SetException(std::make_exception_ptr(SocketException::FromErrno(err)));
  return fut;
}

// ============================================================================
// UDP operations
// ============================================================================

Future<std::tuple<std::vector<uint8_t>, SockAddr>> SockRecvFrom(
    std::shared_ptr<UdpSocket> sock,
    size_t max_len,
    EventLoop* loop) {
  loop = GetLoop(loop);
  if (!loop) {
    Future<std::tuple<std::vector<uint8_t>, SockAddr>> fut;
    fut.SetException(std::make_exception_ptr(
        std::runtime_error("No event loop")));
    return fut;
  }

  std::vector<uint8_t> buf(max_len);
  sockaddr_storage addr_storage{};
  socklen_t addr_len = sizeof(addr_storage);

  ssize_t n = ::recvfrom(sock->Fd(),
                         reinterpret_cast<char*>(buf.data()),
                         max_len, 0,
                         reinterpret_cast<sockaddr*>(&addr_storage),
                         &addr_len);

  if (n >= 0) {
    buf.resize(static_cast<size_t>(n));
    SockAddr addr(reinterpret_cast<sockaddr*>(&addr_storage), addr_len);
    Future<std::tuple<std::vector<uint8_t>, SockAddr>> fut;
    fut.SetResult(std::make_tuple(std::move(buf), std::move(addr)));
    return fut;
  }

  int err = socket_os::GetLastError();
  if (err == EAGAIN || err == EWOULDBLOCK) {
    struct Context {
      std::shared_ptr<UdpSocket> sock;
      size_t max_len;
      Future<std::tuple<std::vector<uint8_t>, SockAddr>> future;
    };

    auto* ctx = new Context{std::move(sock), max_len, {}};

    loop->AddReader(ctx->sock->Fd(), [ctx, loop]() {
      std::vector<uint8_t> buf(ctx->max_len);
      sockaddr_storage addr_storage{};
      socklen_t addr_len = sizeof(addr_storage);

      ssize_t n = ::recvfrom(ctx->sock->Fd(),
                             reinterpret_cast<char*>(buf.data()),
                             ctx->max_len, 0,
                             reinterpret_cast<sockaddr*>(&addr_storage),
                             &addr_len);

      loop->RemoveReader(ctx->sock->Fd());

      if (n >= 0) {
        buf.resize(static_cast<size_t>(n));
        SockAddr addr(reinterpret_cast<sockaddr*>(&addr_storage), addr_len);
        ctx->future.SetResult(std::make_tuple(std::move(buf), std::move(addr)));
      } else {
        ctx->future.SetException(std::make_exception_ptr(
            SocketException::FromLastError()));
      }
      delete ctx;
    });

    return ctx->future;
  }

  Future<std::tuple<std::vector<uint8_t>, SockAddr>> fut;
  fut.SetException(std::make_exception_ptr(SocketException::FromErrno(err)));
  return fut;
}

Future<size_t> SockSendTo(
    std::shared_ptr<UdpSocket> sock,
    std::span<const uint8_t> data,
    const SockAddr& addr,
    EventLoop* loop) {
  loop = GetLoop(loop);
  if (!loop) {
    Future<size_t> fut;
    fut.SetException(std::make_exception_ptr(
        std::runtime_error("No event loop")));
    return fut;
  }

  ssize_t n = ::sendto(sock->Fd(),
                       reinterpret_cast<const char*>(data.data()),
                       data.size(), 0,
                       addr.CAddr(), addr.Len());

  if (n >= 0) {
    Future<size_t> fut;
    fut.SetResult(static_cast<size_t>(n));
    return fut;
  }

  int err = socket_os::GetLastError();
  if (err == EAGAIN || err == EWOULDBLOCK) {
    struct Context {
      std::shared_ptr<UdpSocket> sock;
      std::span<const uint8_t> data;
      SockAddr addr;
      Future<size_t> future;
    };

    auto* ctx = new Context{std::move(sock), data, addr, {}};

    loop->AddWriter(ctx->sock->Fd(), [ctx, loop]() {
      loop->RemoveWriter(ctx->sock->Fd());
      ssize_t n = ::sendto(ctx->sock->Fd(),
                           reinterpret_cast<const char*>(ctx->data.data()),
                           ctx->data.size(), 0,
                           ctx->addr.CAddr(), ctx->addr.Len());
      if (n >= 0) {
        ctx->future.SetResult(static_cast<size_t>(n));
      } else {
        ctx->future.SetException(std::make_exception_ptr(
            SocketException::FromLastError()));
      }
      delete ctx;
    });

    return ctx->future;
  }

  Future<size_t> fut;
  fut.SetException(std::make_exception_ptr(SocketException::FromErrno(err)));
  return fut;
}

}  // namespace socket_ops
}  // namespace asyncio
