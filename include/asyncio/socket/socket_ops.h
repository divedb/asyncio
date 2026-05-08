// Copyright 2025 asyncio-cpp authors. All rights reserved.
// socket_ops.h — Async socket operations.

#ifndef ASYNCIO_SOCKET_SOCKET_OPS_H_
#define ASYNCIO_SOCKET_SOCKET_OPS_H_

#include <cstddef>
#include <memory>
#include <span>
#include <tuple>

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/socket/socket_base.h"

namespace asyncio {

namespace socket_ops {

Future<std::vector<uint8_t>> SockRecv(
    std::shared_ptr<TcpSocket> sock,
    size_t max_len,
    EventLoop* loop = nullptr);

Future<size_t> SockRecvInto(
    std::shared_ptr<TcpSocket> sock,
    std::span<uint8_t> buf,
    EventLoop* loop = nullptr);

Future<size_t> SockSendAll(
    std::shared_ptr<TcpSocket> sock,
    std::span<const uint8_t> data,
    EventLoop* loop = nullptr);

Future<size_t> SockSend(
    std::shared_ptr<TcpSocket> sock,
    std::span<const uint8_t> data,
    EventLoop* loop = nullptr);

Future<void> SockConnect(
    std::shared_ptr<TcpSocket> sock,
    const SockAddr& addr,
    EventLoop* loop = nullptr);

Future<void> SockConnect(
    std::shared_ptr<TcpSocket> sock,
    const std::string& host,
    int port,
    EventLoop* loop = nullptr);

Future<std::tuple<std::shared_ptr<TcpSocket>, SockAddr>> SockAccept(
    std::shared_ptr<TcpListener> listener,
    EventLoop* loop = nullptr);

Future<std::tuple<std::vector<uint8_t>, SockAddr>> SockRecvFrom(
    std::shared_ptr<UdpSocket> sock,
    size_t max_len,
    EventLoop* loop = nullptr);

Future<size_t> SockSendTo(
    std::shared_ptr<UdpSocket> sock,
    std::span<const uint8_t> data,
    const SockAddr& addr,
    EventLoop* loop = nullptr);

}  // namespace socket_ops

// ============================================================================
// TcpSocket method implementations
// ============================================================================

inline Future<void> TcpSocket::Connect(const std::string& host, int port, EventLoop* loop) {
  return socket_ops::SockConnect(shared_from_this(), host, port, loop);
}

inline Future<void> TcpSocket::Connect(const SockAddr& addr, EventLoop* loop) {
  return socket_ops::SockConnect(shared_from_this(), addr, loop);
}

inline Future<std::vector<uint8_t>> TcpSocket::Recv(size_t max_len, EventLoop* loop) {
  return socket_ops::SockRecv(shared_from_this(), max_len, loop);
}

inline Future<size_t> TcpSocket::RecvInto(std::span<uint8_t> buf, EventLoop* loop) {
  return socket_ops::SockRecvInto(shared_from_this(), buf, loop);
}

inline Future<size_t> TcpSocket::SendAll(std::span<const uint8_t> data, EventLoop* loop) {
  return socket_ops::SockSendAll(shared_from_this(), data, loop);
}

inline Future<size_t> TcpSocket::Send(std::span<const uint8_t> data, EventLoop* loop) {
  return socket_ops::SockSend(shared_from_this(), data, loop);
}

// ============================================================================
// TcpListener method implementations
// ============================================================================

inline Future<void> TcpListener::Bind(const std::string& host, int port, EventLoop* loop) {
  auto addr = SockAddr::CreateIPv4(host, port);
  return Bind(addr, loop);
}

inline Future<void> TcpListener::Bind(const SockAddr& addr, EventLoop* /*loop*/) {
  if (!IsValid()) {
    Future<void> fut;
    fut.SetException(std::make_exception_ptr(
        SocketException(EINVAL, "Invalid socket")));
    return fut;
  }

  int err = ::bind(Fd(), addr.CAddr(), addr.Len());
  if (err == kSocketError) {
    Future<void> fut;
    fut.SetException(std::make_exception_ptr(SocketException::FromLastError()));
    return fut;
  }
  Future<void> fut;
  fut.SetResult();
  return fut;
}

inline void TcpListener::Listen(int backlog) {
  if (!IsValid()) return;
  if (::listen(Fd(), backlog) == kSocketError) {
    throw SocketException::FromLastError();
  }
}

inline Future<std::tuple<std::shared_ptr<TcpSocket>, SockAddr>> TcpListener::Accept(
    EventLoop* loop) {
  return socket_ops::SockAccept(shared_from_this(), loop);
}

// ============================================================================
// UdpSocket method implementations
// ============================================================================

inline Future<void> UdpSocket::Bind(const std::string& host, int port, EventLoop* loop) {
  auto addr = SockAddr::CreateIPv4(host, port);
  return Bind(addr, loop);
}

inline Future<void> UdpSocket::Bind(const SockAddr& addr, EventLoop* /*loop*/) {
  if (!IsValid()) {
    Future<void> fut;
    fut.SetException(std::make_exception_ptr(
        SocketException(EINVAL, "Invalid socket")));
    return fut;
  }

  int err = ::bind(Fd(), addr.CAddr(), addr.Len());
  if (err == kSocketError) {
    Future<void> fut;
    fut.SetException(std::make_exception_ptr(SocketException::FromLastError()));
    return fut;
  }
  Future<void> fut;
  fut.SetResult();
  return fut;
}

inline Future<std::tuple<std::vector<uint8_t>, SockAddr>> UdpSocket::RecvFrom(
    size_t max_len, EventLoop* loop) {
  return socket_ops::SockRecvFrom(shared_from_this(), max_len, loop);
}

inline Future<size_t> UdpSocket::SendTo(std::span<const uint8_t> data,
                                        const SockAddr& addr,
                                        EventLoop* loop) {
  return socket_ops::SockSendTo(shared_from_this(), data, addr, loop);
}

inline Future<size_t> UdpSocket::Send(std::span<const uint8_t> data, EventLoop* loop) {
  // For unconnected UDP socket, use sendto with empty address
  // For connected UDP socket, this would work with the connected peer
  loop = loop ? loop : EventLoop::Current();
  if (!loop) {
    Future<size_t> fut;
    fut.SetException(std::make_exception_ptr(
        std::runtime_error("No event loop")));
    return fut;
  }

  ssize_t n = ::send(Fd(),
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
      SocketFd fd;
      std::span<const uint8_t> data;
      Future<size_t> future;
    };

    auto* ctx = new Context{Fd(), data, {}};

    loop->AddWriter(Fd(), [ctx, loop]() {
      loop->RemoveWriter(ctx->fd);
      ssize_t n = ::send(ctx->fd,
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

}  // namespace asyncio

#endif  // ASYNCIO_SOCKET_SOCKET_OPS_H_
