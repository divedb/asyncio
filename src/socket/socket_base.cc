// Copyright 2025 asyncio-cpp authors. All rights reserved.
// socket_base.cc — Implementation of SocketBase and related classes.

#include "asyncio/socket/socket_base.h"

#include <cstring>

#include "asyncio/event_loop.h"

namespace asyncio {

// ============================================================================
// SockAddr
// ============================================================================

SockAddr SockAddr::CreateIPv4(const std::string& host, int port) {
  SockAddr addr;
  auto* sin = reinterpret_cast<sockaddr_in*>(addr.MutableCAddr());
  sin->sin_family = AF_INET;
  sin->sin_port = htons(static_cast<uint16_t>(port));

  if (host.empty() || host == "0.0.0.0") {
    sin->sin_addr.s_addr = INADDR_ANY;
  } else {
    if (inet_pton(AF_INET, host.c_str(), &sin->sin_addr) != 1) {
      addrinfo hints{};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      addrinfo* result = nullptr;
      if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0 && result) {
        auto* result_sin = reinterpret_cast<sockaddr_in*>(result->ai_addr);
        sin->sin_addr = result_sin->sin_addr;
        freeaddrinfo(result);
      }
    }
  }
  addr.MutableLen() = sizeof(sockaddr_in);
  return addr;
}

SockAddr SockAddr::CreateIPv6(const std::string& host, int port) {
  SockAddr addr;
  auto* sin6 = reinterpret_cast<sockaddr_in6*>(addr.MutableCAddr());
  sin6->sin6_family = AF_INET6;
  sin6->sin6_port = htons(static_cast<uint16_t>(port));

  if (host.empty() || host == "::") {
    sin6->sin6_addr = in6addr_any;
  } else {
    if (inet_pton(AF_INET6, host.c_str(), &sin6->sin6_addr) != 1) {
      addrinfo hints{};
      hints.ai_family = AF_INET6;
      hints.ai_socktype = SOCK_STREAM;
      addrinfo* result = nullptr;
      if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0 && result) {
        auto* result_sin6 = reinterpret_cast<sockaddr_in6*>(result->ai_addr);
        sin6->sin6_addr = result_sin6->sin6_addr;
        freeaddrinfo(result);
      }
    }
  }
  addr.MutableLen() = sizeof(sockaddr_in6);
  return addr;
}

int SockAddr::Port() const {
  if (Empty()) return 0;
  auto family = Family();
  if (family == AddressFamily::kIPv4) {
    auto* sin = reinterpret_cast<const sockaddr_in*>(CAddr());
    return ntohs(sin->sin_port);
  } else if (family == AddressFamily::kIPv6) {
    auto* sin6 = reinterpret_cast<const sockaddr_in6*>(CAddr());
    return ntohs(sin6->sin6_port);
  }
  return 0;
}

std::string SockAddr::Address() const {
  if (Empty()) return {};
  char buf[INET6_ADDRSTRLEN] = {};

  auto family = Family();
  if (family == AddressFamily::kIPv4) {
    auto* sin = reinterpret_cast<const sockaddr_in*>(CAddr());
    inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
  } else if (family == AddressFamily::kIPv6) {
    auto* sin6 = reinterpret_cast<const sockaddr_in6*>(CAddr());
    inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf));
  }

  return std::string(buf);
}

// ============================================================================
// SocketBase
// ============================================================================

SocketBase::SocketBase(SocketFd fd) : fd_(fd) {
  if (IsValid()) {
    sockaddr_storage ss{};
    socklen_t sslen = sizeof(ss);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &sslen) == 0) {
      family_ = static_cast<AddressFamily>(ss.ss_family);
    }

    int type = 0;
    socklen_t typelen = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &typelen) == 0) {
      type_ = static_cast<SocketType>(type);
    }
  }
}

SocketBase::~SocketBase() {
  CloseImpl();
}

SocketBase::SocketBase(SocketBase&& other) noexcept
    : fd_(other.fd_),
      family_(other.family_),
      type_(other.type_),
      non_blocking_(other.non_blocking_),
      closing_(other.closing_) {
  other.fd_ = kInvalidSocket;
  other.closing_ = true;
}

SocketBase& SocketBase::operator=(SocketBase&& other) noexcept {
  if (this != &other) {
    CloseImpl();
    fd_ = other.fd_;
    family_ = other.family_;
    type_ = other.type_;
    non_blocking_ = other.non_blocking_;
    closing_ = other.closing_;
    other.fd_ = kInvalidSocket;
    other.closing_ = true;
  }
  return *this;
}

void SocketBase::CloseImpl() {
  if (fd_ != kInvalidSocket && !closing_) {
    closing_ = true;
    OnClose();
    socket_os::Close(fd_);
    fd_ = kInvalidSocket;
  }
}

void SocketBase::Close() {
  CloseImpl();
  closing_ = true;
}

SocketFd SocketBase::Detach() {
  SocketFd fd = fd_;
  fd_ = kInvalidSocket;
  closing_ = true;
  return fd;
}

void SocketBase::Attach(SocketFd fd) {
  CloseImpl();
  fd_ = fd;
  closing_ = false;

  if (IsValid()) {
    sockaddr_storage ss{};
    socklen_t sslen = sizeof(ss);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &sslen) == 0) {
      family_ = static_cast<AddressFamily>(ss.ss_family);
    }

    int type = 0;
    socklen_t typelen = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &typelen) == 0) {
      type_ = static_cast<SocketType>(type);
    }
  }
}

void SocketBase::SetNonBlocking(bool non_blocking) {
  if (!IsValid()) return;
  socket_os::SetNonBlock(fd_);
  non_blocking_ = non_blocking;
}

SockAddr SocketBase::LocalAddress() const {
  if (!IsValid()) return {};
  sockaddr_storage ss{};
  socklen_t sslen = sizeof(ss);
  if (getsockname(fd_, reinterpret_cast<sockaddr*>(&ss), &sslen) == 0) {
    return SockAddr(reinterpret_cast<sockaddr*>(&ss), sslen);
  }
  return {};
}

SockAddr SocketBase::PeerAddress() const {
  if (!IsValid()) return {};
  sockaddr_storage ss{};
  socklen_t sslen = sizeof(ss);
  if (getpeername(fd_, reinterpret_cast<sockaddr*>(&ss), &sslen) == 0) {
    return SockAddr(reinterpret_cast<sockaddr*>(&ss), sslen);
  }
  return {};
}

void SocketBase::ShutdownRead() {
  if (!IsValid()) return;
#ifdef ASYNCIO_OS_WINDOWS
  socket_os::Shutdown(fd_, SD_RECEIVE);
#else
  socket_os::Shutdown(fd_, SHUT_RD);
#endif
}

void SocketBase::ShutdownWrite() {
  if (!IsValid()) return;
#ifdef ASYNCIO_OS_WINDOWS
  socket_os::Shutdown(fd_, SD_SEND);
#else
  socket_os::Shutdown(fd_, SHUT_WR);
#endif
}

void SocketBase::Shutdown() {
  if (!IsValid()) return;
#ifdef ASYNCIO_OS_WINDOWS
  socket_os::Shutdown(fd_, SD_BOTH);
#else
  socket_os::Shutdown(fd_, SHUT_RDWR);
#endif
}

void SocketBase::ReuseAddress(bool reuse) {
  int val = reuse ? 1 : 0;
  SetSockOpt(SocketOptionLevel::kSocket, SocketOption::kReuseAddr, val);
}

void SocketBase::KeepAlive(bool keepalive) {
  int val = keepalive ? 1 : 0;
  SetSockOpt(SocketOptionLevel::kSocket, SocketOption::kKeepAlive, val);
}

// ============================================================================
// TcpSocket
// ============================================================================

std::shared_ptr<TcpSocket> TcpSocket::Create() {
  static SocketInit init;
  auto sock = std::make_shared<TcpSocket>();

  SocketFd fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == kInvalidSocket) {
    throw SocketException::FromLastError();
  }
  sock->Attach(fd);
  return sock;
}

std::shared_ptr<TcpSocket> TcpSocket::CreateFromFd(SocketFd fd) {
  auto sock = std::make_shared<TcpSocket>(fd);
  sock->connected_ = true;
  return sock;
}

void TcpSocket::OnClose() {}

// ============================================================================
// TcpListener
// ============================================================================

std::shared_ptr<TcpListener> TcpListener::Create() {
  static SocketInit init;
  auto listener = std::make_shared<TcpListener>();

  SocketFd fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == kInvalidSocket) {
    throw SocketException::FromLastError();
  }
  listener->Attach(fd);
  return listener;
}

int TcpListener::Port() const {
  return LocalAddress().Port();
}

// ============================================================================
// UdpSocket
// ============================================================================

std::shared_ptr<UdpSocket> UdpSocket::Create() {
  static SocketInit init;
  auto sock = std::make_shared<UdpSocket>();

  SocketFd fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == kInvalidSocket) {
    throw SocketException::FromLastError();
  }
  sock->Attach(fd);
  return sock;
}

std::shared_ptr<UdpSocket> UdpSocket::CreateFromFd(SocketFd fd) {
  return std::make_shared<UdpSocket>(fd);
}

void UdpSocket::OnClose() {}

}  // namespace asyncio
