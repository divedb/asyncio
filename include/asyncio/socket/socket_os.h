// Copyright 2025 asyncio-cpp authors. All rights reserved.
// socket_os.h — compile-time selection of platform socket headers.

#ifndef ASYNCIO_SOCKET_SOCKET_OS_H_
#define ASYNCIO_SOCKET_SOCKET_OS_H_

#include "asyncio/error.h"

#ifdef _WIN32
#define ASYNCIO_OS_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#undef ASYNCIO_OS_WINDOWS
#elif defined(__APPLE__)
#define ASYNCIO_OS_APPLE
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#define ASYNCIO_OS_CLEANUP
#elif defined(__linux__)
#define ASYNCIO_OS_LINUX
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#define ASYNCIO_OS_CLEANUP
#else
#error "Unsupported platform"
#endif

namespace asyncio {

// ---------------------------------------------------------------------------
// Unified socket type aliases
// ---------------------------------------------------------------------------

#ifdef ASYNCIO_OS_WINDOWS
using SocketFd = SOCKET;
constexpr SocketFd kInvalidSocket = INVALID_SOCKET;
constexpr int kSocketError = SOCKET_ERROR;
#else
using SocketFd = int;
constexpr SocketFd kInvalidSocket = -1;
constexpr int kSocketError = -1;
#endif

// ---------------------------------------------------------------------------
// Cross-platform helpers
// ---------------------------------------------------------------------------

namespace socket_os {

inline void Close(SocketFd fd) {
#ifdef ASYNCIO_OS_WINDOWS
  ::closesocket(fd);
#else
  ::close(fd);
#endif
}

inline std::system_error MakeErrorCode(int errc) {
#ifdef ASYNCIO_OS_WINDOWS
  return std::system_error(errc, std::system_category());
#else
  return std::system_error(errc, std::generic_category());
#endif
}

inline void SetNonBlock(SocketFd fd) {
#ifdef ASYNCIO_OS_WINDOWS
  unsigned long mode = 1;
  ::ioctlsocket(fd, FIONBIO, &mode);
#else
  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

inline int GetLastError() {
#ifdef ASYNCIO_OS_WINDOWS
  return WSAGetLastError();
#else
  return errno;
#endif
}

inline int GetSocketError(SocketFd fd) {
#ifdef ASYNCIO_OS_WINDOWS
  int err = 0;
  int len = sizeof(err);
  ::getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
  return err;
#else
  int err = 0;
  socklen_t len = sizeof(err);
  ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
  return err;
#endif
}

inline int Shutdown(SocketFd fd, int how) {
  return ::shutdown(fd, how);
}

}  // namespace socket_os

class SocketInit {
 public:
  SocketInit() {
#ifdef ASYNCIO_OS_WINDOWS
    WSADATA wsa_data;
    int err = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (err != 0) {
      throw std::system_error(err, std::system_category(), "WSAStartup failed");
    }
#endif
  }

  ~SocketInit() {
#ifdef ASYNCIO_OS_WINDOWS
    WSACleanup();
#endif
  }

  SocketInit(const SocketInit&) = delete;
  SocketInit& operator=(const SocketInit&) = delete;
  SocketInit(SocketInit&&) = delete;
  SocketInit& operator=(SocketInit&&) = delete;
};

}  // namespace asyncio

#endif  // ASYNCIO_SOCKET_SOCKET_OS_H_
