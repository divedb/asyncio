#include "asyncio/backend/wakeup_pipe.hh"

#include <array>
#include <system_error>

#if !defined(ASYNCIO_WINDOWS)
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace {

#if defined(ASYNCIO_WINDOWS)

struct WsaGuard {
  WsaGuard() {
    WSADATA wd{};

    if (int r = ::WSAStartup(MAKEWORD(2, 2), &wd); r != 0) {
      throw std::system_error(r, std::system_category(), "WSAStartup");
    }
  }

  ~WsaGuard() { ::WSACleanup(); }
};

void EnsureWinsock() {
  static WsaGuard guard;
  (void)guard;
}

/// \brief Creates a connected pair of sockets suitable for selector wakeup.
///
/// This helper returns two connected stream sockets configured for local,
/// in-process communication. The first socket is the read endpoint and the
/// second socket is the write endpoint.
///
/// On POSIX systems, the implementation typically uses:
///
/// ```cpp
/// socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
/// ```
///
/// which creates two directly connected Unix domain sockets.
///
/// On Windows, Winsock does not provide `socketpair()`, so the implementation
/// emulates it by:
///
/// Algorithm (matches libuv's uv__create_pipe / ZeroMQ / Asio):
///   1. Create a listener TCP socket on 127.0.0.1 with a kernel-assigned port.
///   2. Create the "write" socket and connect() to the listener.
///   3. accept() on the listener to produce the "read" socket.
///   4. Close the listener — it is no longer needed.
///   5. Make both sockets non-blocking (ioctlsocket FIONBIO).
///
/// Why TCP and not UDP?
///   UDP datagrams could be reordered or lost in pathological network stacks.
///   TCP loopback is reliable, ordered, and always available.
///
/// The resulting client and server sockets form a connected pair equivalent to
/// `socketpair()`.
///
/// Both sockets are configured for non-blocking I/O before being returned.
///
/// The returned pair is ordered as:
///
/// - `first`  — read endpoint to register with the selector.
/// - `second` — write endpoint used to signal wakeups.
///
/// Writing one or more bytes to the second socket causes the first socket to
/// become readable, allowing a blocking `select()` call to be interrupted.
///
/// \throws std::system_error if socket creation, binding, listening,
///         connecting, accepting, or non-blocking configuration fails.
void MakeSocketPair(NativeHandle& read_sock, NativeHandle& write_sock) {
  EnsureWinsock();

  SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

  if (listener == INVALID_SOCKET) {
    throw std::system_error(LastError(), std::system_category(), "WakeupPipe: socket (listener)");
  }

  // Bind to 127.0.0.1 with port 0 (kernel picks a free port).
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    CloseHandle(listener);
    throw std::system_error(LastError(), std::system_category(), "WakeupPipe: bind");
  }

  if (::listen(listener, 1) == SOCKET_ERROR) {
    CloseHandle(listener);
    throw std::system_error(LastError(), std::system_category(), "WakeupPipe: listen");
  }

  // Retrieve the port the kernel assigned.
  int addrlen = static_cast<int>(sizeof(addr));

  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrlen) == SOCKET_ERROR) {
    CloseHandle(listener);
    throw std::system_error(LastError(), std::system_category(), "WakeupPipe: getsockname");
  }

  // Create the write end and connect to the listener.
  SOCKET writer = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

  if (writer == INVALID_SOCKET) {
    CloseHandle(listener);
    throw std::system_error(LastError(), std::system_category(), "WakeupPipe: socket (writer)");
  }

  if (::connect(writer, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    CloseHandle(writer);
    CloseHandle(listener);
    throw std::system_error(LastError(), std::system_category(), "WakeupPipe: connect");
  }

  // Accept the read end.
  SOCKET reader = ::accept(listener, nullptr, nullptr);
  CloseHandle(listener);  // listener is done regardless of accept result

  if (reader == INVALID_SOCKET) {
    CloseHandle(writer);
    throw std::system_error(LastError(), std::system_category(), "WakeupPipe: accept");
  }

  // Both ends non-blocking.
  u_long nb = 1;

  if (::ioctlsocket(reader, FIONBIO, &nb) == SOCKET_ERROR ||
      ::ioctlsocket(writer, FIONBIO, &nb) == SOCKET_ERROR) {
    CloseHandle(reader);
    CloseHandle(writer);
    throw std::system_error(LastError(), std::system_category(), "WakeupPipe: ioctlsocket FIONBIO");
  }

  // Disable Nagle on both ends — we send single-byte wakeup tokens and need
  // them to be delivered immediately, not held in the Nagle buffer.
  BOOL no_delay = TRUE;
  ::setsockopt(reader, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay),
               sizeof(no_delay));
  ::setsockopt(writer, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay),
               sizeof(no_delay));

  read_sock = reader;
  write_sock = writer;
}

#else

/// socketpair(AF_UNIX, SOCK_STREAM, 0, sv) creates a bidirectional pair of
/// connected UNIX-domain stream sockets.
///
/// We only use one direction: sv[0] is registered with the selector for reading,
/// and sv[1] is used to write wakeup bytes. SOCK_STREAM provides an ordered,
/// reliable byte stream with no message boundaries, which is exactly what the
/// wakeup pipe needs: one or more writes coalesce into readable state until the
/// read side is drained.
///
/// Non-blocking + close-on-exec:
///   Linux 2.6.27+ accepts SOCK_NONBLOCK | SOCK_CLOEXEC as socket type flags,
///   allowing both attributes to be set atomically when the descriptors are
///   created. This avoids the classic race where another thread could fork+exec
///   before FD_CLOEXEC is applied with fcntl().
///   macOS / BSD do not support these socketpair() type flags, so we fall back
///   to separate fcntl() calls. That fallback is safe as long as no concurrent
///   fork+exec can happen before FD_CLOEXEC is installed.
[[maybe_unused]] void SetNonblockingCloexec(int fd) {
  // FD_CLOEXEC — close on exec(2), prevents the fd leaking into child
  // processes spawned after the event loop is running.
  if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
    throw std::system_error(errno, std::generic_category(), "WakeupPipe: F_SETFD FD_CLOEXEC");
  }

  int flags = ::fcntl(fd, F_GETFL);

  if (flags < 0) {
    throw std::system_error(errno, std::generic_category(), "WakeupPipe: F_GETFL");
  }

  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    throw std::system_error(errno, std::generic_category(), "WakeupPipe: F_SETFL O_NONBLOCK");
  }
}

void MakeSocketPair(NativeHandle& read_sock, NativeHandle& write_sock) {
  int sv[2]{-1, -1};

  // Attempt atomic SOCK_NONBLOCK | SOCK_CLOEXEC (Linux 2.6.27+).
  // Fall back gracefully to plain socketpair + separate fcntl() on platforms
  // that don't support the flags (macOS, older Linux, BSDs).
#if defined(__linux__)
  // Linux: set flags atomically inside socketpair() — no TOCTTOU window.
  int rc = ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv);
  if (rc < 0) throw std::system_error(errno, std::generic_category(), "WakeupPipe: socketpair");
  // Flags already set; skip the fcntl() fallback below.
  read_sock = sv[0];
  write_sock = sv[1];
#else
  // macOS / BSD: plain socketpair, then set flags via fcntl().
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    throw std::system_error(errno, std::generic_category(), "WakeupPipe: socketpair");

  try {
    SetNonblockingCloexec(sv[0]);
    SetNonblockingCloexec(sv[1]);
  } catch (...) {
    CloseHandle(sv[0]);
    CloseHandle(sv[1]);
    throw;
  }

  read_sock = sv[0];
  write_sock = sv[1];
#endif
}

#endif  // ASYNCIO_WINDOWS / ASYNCIO_POSIX

}  // namespace

namespace asyncio {

WakeupPipe::WakeupPipe() { MakeSocketPair(read_handle_, write_handle_); }

WakeupPipe::~WakeupPipe() {
  CloseHandle(write_handle_);
  CloseHandle(read_handle_);
}

void WakeupPipe::Wakeup() {
  // Send a single arbitrary byte.  The value doesn't matter; the selector
  // only needs the fd to become readable.
  const char byte = '\x01';

#if defined(ASYNCIO_WINDOWS)
  int r = ::send(write_handle_, &byte, 1, 0);
  if (r == SOCKET_ERROR) {
    int err = LastError();
    if (!IsWouldBlock(err))
      throw std::system_error(err, std::system_category(), "WakeupPipe::Wakeup send");
    // WSAEWOULDBLOCK: the send buffer is full, meaning at least one wakeup
    // byte is already queued — the reader will be woken regardless.
  }
#else
  // MSG_NOSIGNAL: suppress SIGPIPE if the read end was closed.
  // On macOS, MSG_NOSIGNAL is not available; use SO_NOSIGPIPE (set once on
  // the socket) or handle SIGPIPE at the process level.
#if defined(MSG_NOSIGNAL)
  ssize_t r = ::send(write_handle_, &byte, 1, MSG_NOSIGNAL);
#else
  ssize_t r = ::write(write_handle_, &byte, 1);
#endif
  if (r < 0) {
    int err = errno;

    if (!IsWouldBlock(err)) {
      // EAGAIN: send buffer is full; a byte is already pending — no action needed.
      throw std::system_error(err, std::generic_category(), "WakeupPipe::Wakeup write");
    }
  }
#endif
}

void WakeupPipe::Drain() {
  // Read and discard all pending bytes in a tight loop until EAGAIN /
  // WSAEWOULDBLOCK, ensuring the handle returns to a non-readable state even
  // if Wakeup() was called multiple times between two Drain() calls.
  //
  // Buffer size: 64 bytes amortises the loop overhead when many Wakeup()
  // calls have accumulated, while keeping the stack footprint trivial.
  std::array<char, 64> buf{};

  for (;;) {
#if defined(ASYNCIO_WINDOWS)
    int n = ::recv(read_handle_, buf.data(), static_cast<int>(buf.size()), 0);

    if (n == SOCKET_ERROR) {
      int err = LastError();

      if (IsWouldBlock(err)) break;  // drained
      // Unexpected error — not fatal for a wakeup drain; stop silently.
      break;
    }
#else
    ssize_t n = ::recv(read_handle_, buf.data(), buf.size(), 0);

    if (n < 0) {
      if (IsWouldBlock(errno)) break;  // drained
      break;
    }
#endif
    if (n == 0) break;  // peer closed (shouldn't happen; write end is ours)
  }
}

}  // namespace asyncio