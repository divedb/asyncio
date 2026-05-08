// Copyright 2025 asyncio-cpp authors. All rights reserved.
// socket_base.h — Base class and common definitions for Socket I/O.

#ifndef ASYNCIO_SOCKET_SOCKET_BASE_H_
#define ASYNCIO_SOCKET_SOCKET_BASE_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>

#include "asyncio/error.h"
#include "asyncio/future.h"
#include "asyncio/socket/socket_os.h"

namespace asyncio {

class EventLoop;

// ---------------------------------------------------------------------------
// Address Family & Socket Type
// ---------------------------------------------------------------------------

enum class AddressFamily {
  kUnspecified = AF_UNSPEC,
  kUnix = AF_UNIX,
  kIPv4 = AF_INET,
  kIPv6 = AF_INET6,
};

enum class SocketType {
  kStream = SOCK_STREAM,
  kDatagram = SOCK_DGRAM,
  kRaw = SOCK_RAW,
};

enum class SocketOptionLevel {
  kSocket = SOL_SOCKET,
  kIP = IPPROTO_IP,
  kTCP = IPPROTO_TCP,
  kIPv6 = IPPROTO_IPV6,
};

enum class SocketOption {
  kReuseAddr = SO_REUSEADDR,
  kReusePort = SO_REUSEPORT,
  kKeepAlive = SO_KEEPALIVE,
  kLinger = SO_LINGER,
  kOobInline = SO_OOBINLINE,
  kSndBuf = SO_SNDBUF,
  kRcvBuf = SO_RCVBUF,
  kSndLowat = SO_SNDLOWAT,
  kRcvLowat = SO_RCVLOWAT,
  kSndTimeo = SO_SNDTIMEO,
  kRcvTimeo = SO_RCVTIMEO,
  kError = SO_ERROR,
  kType = SO_TYPE,
  kTcpNoDelay = TCP_NODELAY,
  kIpMulticastLoop = IP_MULTICAST_LOOP,
  kIpMulticastTtl = IP_MULTICAST_TTL,
  kIpV6Only = IPV6_V6ONLY,
};

// ---------------------------------------------------------------------------
// SockAddr
// ---------------------------------------------------------------------------

class SockAddr {
 public:
  SockAddr() : len_(sizeof(storage_)) {}

  explicit SockAddr(const sockaddr* addr, socklen_t len) : len_(len) {
    if (addr && len > 0 && len <= sizeof(storage_)) {
      std::memcpy(&storage_, addr, len);
    }
  }

  static SockAddr CreateIPv4(const std::string& host, int port);
  static SockAddr CreateIPv6(const std::string& host, int port);

  AddressFamily Family() const {
    if (Empty()) return AddressFamily::kUnspecified;
    return static_cast<AddressFamily>(CAddr()->sa_family);
  }

  int Port() const;
  std::string Address() const;

  const sockaddr* CAddr() const { return reinterpret_cast<const sockaddr*>(&storage_); }
  sockaddr* MutableCAddr() { return reinterpret_cast<sockaddr*>(&storage_); }
  socklen_t Len() const { return len_; }
  socklen_t& MutableLen() { return len_; }
  bool Empty() const { return len_ == 0; }

 private:
  sockaddr_storage storage_;
  socklen_t len_ = 0;
};

// ---------------------------------------------------------------------------
// SocketException
// ---------------------------------------------------------------------------

class SocketException : public std::system_error {
 public:
  SocketException(int errc, const std::string& what)
      : std::system_error(errc, std::system_category(), what) {}

  SocketException(int errc, const char* what)
      : std::system_error(errc, std::system_category(), what) {}

  static SocketException FromErrno(int errc) {
    return SocketException(errc, std::strerror(errc));
  }

  static SocketException FromLastError() {
    return FromErrno(socket_os::GetLastError());
  }
};

// ---------------------------------------------------------------------------
// SocketBase
// ---------------------------------------------------------------------------

class SocketBase : public std::enable_shared_from_this<SocketBase> {
 public:
  SocketBase() = default;
  explicit SocketBase(SocketFd fd);
  virtual ~SocketBase();

  SocketBase(const SocketBase&) = delete;
  SocketBase& operator=(const SocketBase&) = delete;
  SocketBase(SocketBase&& other) noexcept;
  SocketBase& operator=(SocketBase&& other) noexcept;

  [[nodiscard]] SocketFd Fd() const { return fd_; }
  [[nodiscard]] bool IsValid() const { return fd_ != kInvalidSocket; }
  [[nodiscard]] explicit operator bool() const { return IsValid(); }
  [[nodiscard]] AddressFamily Family() const { return family_; }
  [[nodiscard]] SocketType Type() const { return type_; }
  [[nodiscard]] bool IsNonBlocking() const { return non_blocking_; }

  [[nodiscard]] SockAddr LocalAddress() const;
  [[nodiscard]] SockAddr PeerAddress() const;

  void Close();
  SocketFd Detach();
  void Attach(SocketFd fd);
  void SetNonBlocking(bool non_blocking = true);

  template <typename T>
  void SetSockOpt(SocketOptionLevel level, SocketOption option, T value);

  template <typename T>
  T GetSockOpt(SocketOptionLevel level, SocketOption option);

  void ReuseAddress(bool reuse = true);
  void KeepAlive(bool keepalive = true);

  void ShutdownRead();
  void ShutdownWrite();
  void Shutdown();

 protected:
  virtual void OnClose() {}
  virtual bool CanClose() { return true; }

  SocketFd fd_ = kInvalidSocket;
  AddressFamily family_ = AddressFamily::kUnspecified;
  SocketType type_ = SocketType::kStream;
  bool non_blocking_ = false;
  bool closing_ = false;

 private:
  void CloseImpl();
};

// ---------------------------------------------------------------------------
// Template implementations
// ---------------------------------------------------------------------------

template <typename T>
void SocketBase::SetSockOpt(SocketOptionLevel level, SocketOption option, T value) {
  if (!IsValid()) return;
  int level_i = static_cast<int>(level);
  int opt_i = static_cast<int>(option);
#ifdef ASYNCIO_OS_WINDOWS
  ::setsockopt(fd_, level_i, opt_i, reinterpret_cast<const char*>(&value), sizeof(value));
#else
  ::setsockopt(fd_, level_i, opt_i, &value, sizeof(value));
#endif
}

template <typename T>
T SocketBase::GetSockOpt(SocketOptionLevel level, SocketOption option) {
  T value{};
  if (!IsValid()) return value;
  int level_i = static_cast<int>(level);
  int opt_i = static_cast<int>(option);
  socklen_t len = sizeof(value);
#ifdef ASYNCIO_OS_WINDOWS
  ::getsockopt(fd_, level_i, opt_i, reinterpret_cast<char*>(&value), &len);
#else
  ::getsockopt(fd_, level_i, opt_i, &value, &len);
#endif
  return value;
}

// ---------------------------------------------------------------------------
// TcpSocket
// ---------------------------------------------------------------------------

class TcpSocket : public SocketBase {
 public:
  static std::shared_ptr<TcpSocket> Create();
  static std::shared_ptr<TcpSocket> CreateFromFd(SocketFd fd);

  TcpSocket() = default;
  explicit TcpSocket(SocketFd fd) : SocketBase(fd) {}

  Future<void> Connect(const std::string& host, int port, EventLoop* loop = nullptr);
  Future<void> Connect(const SockAddr& addr, EventLoop* loop = nullptr);

  [[nodiscard]] bool IsConnected() const { return connected_; }

  Future<std::vector<uint8_t>> Recv(size_t max_len, EventLoop* loop = nullptr);
  Future<size_t> RecvInto(std::span<uint8_t> buf, EventLoop* loop = nullptr);
  Future<size_t> SendAll(std::span<const uint8_t> data, EventLoop* loop = nullptr);
  Future<size_t> Send(std::span<const uint8_t> data, EventLoop* loop = nullptr);

  std::shared_ptr<TcpSocket> shared_from_this() {
    return std::static_pointer_cast<TcpSocket>(SocketBase::shared_from_this());
  }

 protected:
  void OnClose() override;

 private:
  bool connected_ = false;
};

// ---------------------------------------------------------------------------
// TcpListener
// ---------------------------------------------------------------------------

class TcpListener : public SocketBase {
 public:
  static std::shared_ptr<TcpListener> Create();

  TcpListener() = default;
  explicit TcpListener(SocketFd fd) : SocketBase(fd) {}

  Future<void> Bind(const std::string& host, int port, EventLoop* loop = nullptr);
  Future<void> Bind(const SockAddr& addr, EventLoop* loop = nullptr);
  void Listen(int backlog = 128);

  Future<std::tuple<std::shared_ptr<TcpSocket>, SockAddr>> Accept(EventLoop* loop = nullptr);
  [[nodiscard]] int Port() const;

  std::shared_ptr<TcpListener> shared_from_this() {
    return std::static_pointer_cast<TcpListener>(SocketBase::shared_from_this());
  }
};

// ---------------------------------------------------------------------------
// UdpSocket
// ---------------------------------------------------------------------------

class UdpSocket : public SocketBase {
 public:
  static std::shared_ptr<UdpSocket> Create();
  static std::shared_ptr<UdpSocket> CreateFromFd(SocketFd fd);

  UdpSocket() = default;
  explicit UdpSocket(SocketFd fd) : SocketBase(fd) {}

  Future<void> Bind(const std::string& host, int port, EventLoop* loop = nullptr);
  Future<void> Bind(const SockAddr& addr, EventLoop* loop = nullptr);

  Future<std::tuple<std::vector<uint8_t>, SockAddr>> RecvFrom(size_t max_len, EventLoop* loop = nullptr);
  Future<size_t> SendTo(std::span<const uint8_t> data, const SockAddr& addr, EventLoop* loop = nullptr);
  Future<size_t> Send(std::span<const uint8_t> data, EventLoop* loop = nullptr);

  std::shared_ptr<UdpSocket> shared_from_this() {
    return std::static_pointer_cast<UdpSocket>(SocketBase::shared_from_this());
  }

 protected:
  void OnClose() override;
};

}  // namespace asyncio

#endif  // ASYNCIO_SOCKET_SOCKET_BASE_H_
