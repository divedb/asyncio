#pragma once

#include <array>
#include <unordered_map>

#include "asyncio/backend/selector.hh"

#if defined(ASYNCIO_OS_WINDOWS)
// winsock2.h already included via selector.h
#include <mstcpip.h>
#else
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace asyncio {

/// Selector implementation backed by POSIX select().
///
/// This is the portable fallback used when neither epoll nor kqueue is
/// available. select() is limited to FD_SETSIZE (typically 1024) file
/// descriptors and has O(n) scan time.
///
/// On macOS / Linux, prefer KqueueSelector or EpollSelector respectively.
class SelectSelector final : public Selector {
 public:
  SelectSelector() { CreateWakeupPipe(); }

  ~SelectSelector() override { CloseWakeupPipe(); }

  void Register(NativeHandle handle, IoEventFlags events, void* user_data) override {
    CheckHandle(handle, "Register");

    if (entries_.count(handle) > 0) {
      throw std::invalid_argument("SelectSelector::Register: handle already registered");
    }

#if defined(ASYNCIO_OS_WINDOWS)
    // https://learn.microsoft.com/en-us/windows/win32/api/winsock/ns-winsock-fd_set
    // typedef struct fd_set {
    //   u_int fd_count;
    //   SOCKET fd_array[FD_SETSIZE];
    // } fd_set, FD_SET, *PFD_SET, *LPFD_SET;
    //
    // Note: windows fd_set does not use bitmasks and instead stores SOCKET handles in an array, but
    // it still has a fixed maximum capacity defined by FD_SETSIZE. The behavior is undefined if
    // fd_count exceeds FD_SETSIZE, which is typically 64 on Windows. Reserve one slot for the
    // internal wakeup socket.

    if (entries_.size() >= FD_SETSIZE - 1)
      throw std::runtime_error("SelectSelector::Register: FD_SETSIZE limit reached on Windows");
#else
    // The behavior of these macros is undefined if a descriptor value is less than zero or greater
    // than or equal to FD_SETSIZE, which is normally at least equal to the maximum number of
    // descriptors supported by the system.

    if (handle >= FD_SETSIZE)
      throw std::runtime_error(
          "SelectSelector::Register: fd exceeds FD_SETSIZE; "
          "use EpollSelector instead");
#endif

    entries_[handle] = {events, user_data};
  }

  void Modify(NativeHandle handle, IoEventFlags events) override {
    auto it = RequireRegistered(handle, "Modify");
    it->second.events = events;
  }

  void ModifyUserData(NativeHandle handle, void* user_data) override {
    auto it = RequireRegistered(handle, "ModifyUserData");
    it->second.user_data = user_data;
  }

  void Unregister(NativeHandle handle) override { entries_.erase(handle); }

  [[nodiscard]] size_t Count() const noexcept override { return entries_.size(); }

  void Interrupt() override {
    static constexpr char kWakeByte = 1;

#if defined(ASYNCIO_OS_WINDOWS)
    const int result = ::send(wakeup_write_handle_, &kWakeByte, 1, 0);
    if (result == 1) {
      return;
    }

    const int error = ::WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
      return;
    }

    throw std::system_error(error, std::system_category(),
                            "SelectSelector::Interrupt send failed");
#else
    const ssize_t result = ::write(wakeup_write_handle_, &kWakeByte, 1);
    if (result == 1) {
      return;
    }

    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }

    throw std::system_error(errno, std::generic_category(),
                            "SelectSelector::Interrupt write failed");
#endif
  }

  [[nodiscard]] const char* BackendName() const noexcept override { return "select"; }

  [[nodiscard]] SelectorCapabilities Capabilities() const noexcept override {
    SelectorCapabilities caps;
    caps.level_triggered = true;
    caps.wakeup = true;
#if defined(ASYNCIO_OS_WINDOWS)
    caps.max_handles = FD_SETSIZE > 0 ? static_cast<size_t>(FD_SETSIZE - 1) : 0;
#endif
    return caps;
  }

 private:
  struct Entry {
    IoEventFlags events{IoEventFlags::kNone};
    void* user_data{nullptr};
  };

  using EntryMap = std::unordered_map<NativeHandle, Entry>;

  /// \brief Checks if the given handle is valid.
  ///
  /// \param handle The native handle to check.
  /// \param ctx    The context in which the check is performed.
  /// \throws       std::invalid_argument if the handle is invalid.
  static void CheckHandle(NativeHandle handle, const char* ctx) {
    if (handle == kInvalidHandle) {
      throw std::invalid_argument(std::string("SelectSelector::") + ctx + ": invalid handle");
    }
  }

  /// \brief Retrieves an iterator to the entry for the given handle, ensuring it is registered.
  ///
  /// \param h   The native handle whose entry is to be retrieved.
  /// \param ctx The context in which the check is performed.
  /// \return    An iterator to the entry corresponding to the given handle.
  /// \throws    std::invalid_argument if the handle is not currently registered in the selector.
  EntryMap::iterator RequireRegistered(NativeHandle h, const char* ctx) {
    auto it = entries_.find(h);

    if (it == entries_.end()) {
      throw std::invalid_argument(std::string("SelectSelector::") + ctx +
                                  ": handle not registered");
    }

    return it;
  }

  [[nodiscard]] static bool WantsRead(IoEventFlags events) noexcept {
    return (events & (IoEventFlags::kReadable | IoEventFlags::kHangup)) != IoEventFlags::kNone;
  }

  [[nodiscard]] static bool WantsWrite(IoEventFlags events) noexcept {
    return (events & IoEventFlags::kWritable) != IoEventFlags::kNone;
  }

  [[nodiscard]] static bool WantsExcept(IoEventFlags events) noexcept {
    return (events & (IoEventFlags::kError | IoEventFlags::kHangup)) != IoEventFlags::kNone;
  }

  int SelectImpl(std::span<IoEvent> out,
                 std::optional<std::chrono::nanoseconds> timeout) override {
    fd_set read_fds;
    fd_set write_fds;
    fd_set except_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_ZERO(&except_fds);

    FD_SET(wakeup_read_handle_, &read_fds);
    NativeHandle max_handle = wakeup_read_handle_;

    for (const auto& [handle, entry] : entries_) {
      if (WantsRead(entry.events)) {
        FD_SET(handle, &read_fds);
      }
      if (WantsWrite(entry.events)) {
        FD_SET(handle, &write_fds);
      }
      if (WantsExcept(entry.events)) {
        FD_SET(handle, &except_fds);
      }
      if (handle > max_handle) {
        max_handle = handle;
      }
    }

    timeval tv_storage{};
    timeval* tv_ptr = nullptr;
    if (timeout.has_value()) {
      const auto clamped = std::max(timeout.value(), std::chrono::nanoseconds::zero());
      const auto secs = std::chrono::duration_cast<std::chrono::seconds>(clamped);
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(clamped - secs);
      tv_storage.tv_sec = static_cast<decltype(tv_storage.tv_sec)>(secs.count());
      tv_storage.tv_usec = static_cast<decltype(tv_storage.tv_usec)>(micros.count());
      tv_ptr = &tv_storage;
    }

#if defined(ASYNCIO_OS_WINDOWS)
    const int ready = ::select(0, &read_fds, &write_fds, &except_fds, tv_ptr);
    if (ready == SOCKET_ERROR) {
      throw std::system_error(::WSAGetLastError(), std::system_category(),
                              "SelectSelector::Select select failed");
    }
#else
    const int ready = ::select(static_cast<int>(max_handle + 1), &read_fds, &write_fds, &except_fds,
                               tv_ptr);
    if (ready < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "SelectSelector::Select select failed");
    }
#endif

    if (ready == 0) {
      return 0;
    }

    if (FD_ISSET(wakeup_read_handle_, &read_fds)) {
      DrainWakeupPipe();
    }

    int produced = 0;
    for (const auto& [handle, entry] : entries_) {
      if (produced >= static_cast<int>(out.size())) {
        break;
      }

      IoEventFlags triggered = IoEventFlags::kNone;
      if (FD_ISSET(handle, &read_fds)) {
        triggered |= IoEventFlags::kReadable;
      }
      if (FD_ISSET(handle, &write_fds)) {
        triggered |= IoEventFlags::kWritable;
      }
      if (FD_ISSET(handle, &except_fds)) {
        triggered |= IoEventFlags::kError;
      }

      if (triggered == IoEventFlags::kNone) {
        continue;
      }

      out[produced++] = IoEvent{handle, triggered, entry.user_data};
    }

    return produced;
  }

  void CreateWakeupPipe() {
#if defined(ASYNCIO_OS_WINDOWS)
    SOCKET listener = INVALID_SOCKET;
    SOCKET writer = INVALID_SOCKET;
    SOCKET reader = INVALID_SOCKET;

    try {
      listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (listener == INVALID_SOCKET) {
        throw std::system_error(::WSAGetLastError(), std::system_category(),
                                "SelectSelector::CreateWakeupPipe socket failed");
      }

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = 0;

      if (::bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        throw std::system_error(::WSAGetLastError(), std::system_category(),
                                "SelectSelector::CreateWakeupPipe bind failed");
      }

      if (::listen(listener, 1) == SOCKET_ERROR) {
        throw std::system_error(::WSAGetLastError(), std::system_category(),
                                "SelectSelector::CreateWakeupPipe listen failed");
      }

      sockaddr_in bound_addr{};
      int bound_len = sizeof(bound_addr);
      if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound_addr), &bound_len) ==
          SOCKET_ERROR) {
        throw std::system_error(::WSAGetLastError(), std::system_category(),
                                "SelectSelector::CreateWakeupPipe getsockname failed");
      }

      writer = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (writer == INVALID_SOCKET) {
        throw std::system_error(::WSAGetLastError(), std::system_category(),
                                "SelectSelector::CreateWakeupPipe socket failed");
      }

      if (::connect(writer, reinterpret_cast<const sockaddr*>(&bound_addr), sizeof(bound_addr)) ==
          SOCKET_ERROR) {
        throw std::system_error(::WSAGetLastError(), std::system_category(),
                                "SelectSelector::CreateWakeupPipe connect failed");
      }

      reader = ::accept(listener, nullptr, nullptr);
      if (reader == INVALID_SOCKET) {
        throw std::system_error(::WSAGetLastError(), std::system_category(),
                                "SelectSelector::CreateWakeupPipe accept failed");
      }

      u_long non_blocking = 1;
      if (::ioctlsocket(reader, FIONBIO, &non_blocking) == SOCKET_ERROR ||
          ::ioctlsocket(writer, FIONBIO, &non_blocking) == SOCKET_ERROR) {
        throw std::system_error(::WSAGetLastError(), std::system_category(),
                                "SelectSelector::CreateWakeupPipe ioctlsocket failed");
      }

      wakeup_read_handle_ = reader;
      wakeup_write_handle_ = writer;
      ::closesocket(listener);
    } catch (...) {
      if (listener != INVALID_SOCKET) {
        ::closesocket(listener);
      }
      if (reader != INVALID_SOCKET) {
        ::closesocket(reader);
      }
      if (writer != INVALID_SOCKET) {
        ::closesocket(writer);
      }
      throw;
    }
#else
    int fds[2];
#if defined(ASYNCIO_OS_LINUX) && defined(O_CLOEXEC) && defined(O_NONBLOCK)
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
      wakeup_read_handle_ = fds[0];
      wakeup_write_handle_ = fds[1];
      return;
    }
    if (errno != ENOSYS && errno != EINVAL) {
      throw std::system_error(errno, std::generic_category(),
                              "SelectSelector::CreateWakeupPipe pipe2 failed");
    }
#endif
    if (::pipe(fds) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "SelectSelector::CreateWakeupPipe pipe failed");
    }

    try {
      SetNonBlocking(fds[0]);
      SetNonBlocking(fds[1]);
      SetCloseOnExec(fds[0]);
      SetCloseOnExec(fds[1]);
    } catch (...) {
      ::close(fds[0]);
      ::close(fds[1]);
      throw;
    }

    wakeup_read_handle_ = fds[0];
    wakeup_write_handle_ = fds[1];
#endif
  }

  void DrainWakeupPipe() noexcept {
#if defined(ASYNCIO_OS_WINDOWS)
    std::array<char, 256> buffer{};
    while (true) {
      const int result = ::recv(wakeup_read_handle_, buffer.data(), static_cast<int>(buffer.size()), 0);
      if (result > 0) {
        continue;
      }
      if (result == 0) {
        break;
      }
      const int error = ::WSAGetLastError();
      if (error == WSAEWOULDBLOCK) {
        break;
      }
      break;
    }
#else
    std::array<char, 256> buffer{};
    while (true) {
      const ssize_t result = ::read(wakeup_read_handle_, buffer.data(), buffer.size());
      if (result > 0) {
        continue;
      }
      if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      }
      break;
    }
#endif
  }

  void CloseWakeupPipe() noexcept {
#if defined(ASYNCIO_OS_WINDOWS)
    if (wakeup_read_handle_ != kInvalidHandle) {
      ::closesocket(wakeup_read_handle_);
      wakeup_read_handle_ = kInvalidHandle;
    }
    if (wakeup_write_handle_ != kInvalidHandle) {
      ::closesocket(wakeup_write_handle_);
      wakeup_write_handle_ = kInvalidHandle;
    }
#else
    if (wakeup_read_handle_ != kInvalidHandle) {
      ::close(wakeup_read_handle_);
      wakeup_read_handle_ = kInvalidHandle;
    }
    if (wakeup_write_handle_ != kInvalidHandle) {
      ::close(wakeup_write_handle_);
      wakeup_write_handle_ = kInvalidHandle;
    }
#endif
  }

#if !defined(ASYNCIO_OS_WINDOWS)
  static void SetNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "SelectSelector::SetNonBlocking F_GETFL failed");
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "SelectSelector::SetNonBlocking F_SETFL failed");
    }
  }

  static void SetCloseOnExec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "SelectSelector::SetCloseOnExec F_GETFD failed");
    }
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "SelectSelector::SetCloseOnExec F_SETFD failed");
    }
  }
#endif

  EntryMap entries_;
  NativeHandle wakeup_read_handle_{kInvalidHandle};
  NativeHandle wakeup_write_handle_{kInvalidHandle};
};

}  // namespace asyncio
