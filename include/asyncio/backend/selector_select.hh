#pragma once

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#include "asyncio/backend/selector.hh"
#include "asyncio/backend/wakeup_pipe.hh"

#if defined(ASYNCIO_OS_WINDOWS)
// winsock2.h already included via selector.h
#include <mstcpip.h>
#else
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace asyncio {

/// \brief Selector implementation backed by POSIX select().
///
/// This is the portable fallback used when neither epoll nor kqueue is
/// available. select() is limited to FD_SETSIZE (typically 1024) file
/// descriptors and has O(n) scan time.
///
/// On macOS / Linux, prefer KqueueSelector or EpollSelector respectively.
class SelectSelector final : public Selector {
 public:
  SelectSelector() = default;
  ~SelectSelector() override = default;

  void Register(NativeHandle handle, IoEventFlags events, void* user_data) override {
    CheckHandle(handle, "Register");

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

    if (entries_.size() >= FD_SETSIZE - 1) {
      throw std::runtime_error("SelectSelector::Register: FD_SETSIZE limit reached on Windows");
    }

#else
    // The behavior of these macros is undefined if a descriptor value is less than zero or greater
    // than or equal to FD_SETSIZE, which is normally at least equal to the maximum number of
    // descriptors supported by the system.

    if (handle >= FD_SETSIZE) {
      throw std::runtime_error(
          "SelectSelector::Register: fd exceeds FD_SETSIZE; "
          "use EpollSelector instead");
    }

#endif

    auto [_, inserted] = entries_.try_emplace(handle, events, user_data);

    if (!inserted) {
      throw std::invalid_argument("SelectSelector::Register: handle already registered");
    }
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

  void Interrupt() override { wakeup_pipe_.Wakeup(); }

  [[nodiscard]] const char* BackendName() const noexcept override { return "select"; }

  [[nodiscard]] SelectorCapabilities Capabilities() const noexcept override {
    SelectorCapabilities caps;
    caps.level_triggered = true;
    caps.wakeup = true;
    caps.max_handles = FD_SETSIZE > 0 ? static_cast<size_t>(FD_SETSIZE - 1) : 0;

    return caps;
  }

 private:
  struct FdSets {
    fd_set read{};
    fd_set write{};
    fd_set except{};

    FdSets() {
      FD_ZERO(&read);
      FD_ZERO(&write);
      FD_ZERO(&except);
    }

    void AddRead(NativeHandle fd) noexcept { FD_SET(fd, &read); }
    void AddWrite(NativeHandle fd) noexcept { FD_SET(fd, &write); }
    void AddExcept(NativeHandle fd) noexcept { FD_SET(fd, &except); }

    bool IsReadable(int fd) const noexcept { return FD_ISSET(fd, &read); }
    bool IsWritable(int fd) const noexcept { return FD_ISSET(fd, &write); }
    bool IsExcept(int fd) const noexcept { return FD_ISSET(fd, &except); }
  };

  struct Entry {
    IoEventFlags events{IoEventFlags::kNone};
    void* user_data{nullptr};
  };

  using EntryMap = std::unordered_map<NativeHandle, Entry>;

  /// \brief Builds a timeval structure from the given timeout.
  ///
  /// \param timeout The optional timeout value.
  /// \param storage The timeval structure to populate.
  /// \return        A pointer to the populated timeval structure, or nullptr if no timeout is
  ///                specified.
  static timeval* BuildTimeout(std::optional<std::chrono::nanoseconds> timeout, timeval& storage) {
    if (!timeout.has_value()) return nullptr;

    const auto clamped = std::max(*timeout, std::chrono::nanoseconds::zero());
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(clamped);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(clamped - secs);

    storage.tv_sec = static_cast<decltype(storage.tv_sec)>(secs.count());
    storage.tv_usec = static_cast<decltype(storage.tv_usec)>(micros.count());

    return &storage;
  }

  /// \brief Calls the select() system call with the given fd_sets and timeout, handling EINTR
  ///        appropriately.
  ///
  /// \param fds        The FdSets structure containing the file descriptors to monitor.
  /// \param max_handle The maximum handle value.
  /// \param tv_ptr     A pointer to the timeval structure specifying the timeout.
  /// \return           The number of ready file descriptors.
  static int CallSelect(FdSets& fds, NativeHandle max_handle, timeval* tv_ptr) {
#if defined(ASYNCIO_OS_WINDOWS)
    const int ready = ::select(0, &fds.read, &fds.write, &fds.except, tv_ptr);

    if (ready == SOCKET_ERROR) {
      throw std::system_error(LastError(), std::system_category(),
                              "SelectSelector::Select select failed");
    }

#else
    // Retry on EINTR — select() can be interrupted by signals (e.g. SIGCHLD,
    // debugger attach, etc.). On timeout we loop only if the caller specified
    // a deadline, preserving the original remaining time for each retry.
    int ready;
    const int max_fd = static_cast<int>(max_handle) + 1;

    for (;;) {
      ready = ::select(max_fd, &fds.read, &fds.write, &fds.except, tv_ptr);

      if (ready >= 0) break;
      if (errno == EINTR) continue;

      throw std::system_error(errno, std::system_category(),
                              "SelectSelector::Select select failed");
    }

#endif
    return ready;
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

  /// \brief Populates the given fd_set structures based on the registered entries and updates
  ///        max_handle.
  ///
  /// \param read_fds   The fd_set to populate with read events.
  /// \param write_fds  The fd_set to populate with write events.
  /// \param except_fds The fd_set to populate with except events.
  /// \param max_handle The current maximum handle value, which will be updated if higher handles
  ///                   are found.
  void PopulateFdSets(FdSets& fds, NativeHandle& max_handle) const noexcept {
    for (const auto& [handle, entry] : entries_) {
      if (WantsRead(entry.events)) fds.AddRead(handle);
      if (WantsWrite(entry.events)) fds.AddWrite(handle);
      if (WantsExcept(entry.events)) fds.AddExcept(handle);
      if (handle > max_handle) max_handle = handle;
    }
  }

  /// \brief Determines the triggered I/O events for the given handle.
  ///
  /// \param fds        The FdSets structure containing the results from select().
  /// \param handle     The native handle to check.
  /// \return           The triggered I/O event flags.
  static IoEventFlags GetTriggeredFlags(const FdSets& fds, NativeHandle handle) noexcept {
    IoEventFlags flags = IoEventFlags::kNone;

    if (fds.IsReadable(handle)) flags |= IoEventFlags::kReadable;
    if (fds.IsWritable(handle)) flags |= IoEventFlags::kWritable;
    if (fds.IsExcept(handle)) flags |= IoEventFlags::kError;

    return flags;
  }

  /// \brief Collects triggered events into the output span based on the fd_set results.
  ///
  /// \param fds The FdSets structure containing the results from select().
  /// \param out The output span to populate with triggered events.
  /// \return    The number of events collected into the output span.
  int CollectEvents(const FdSets& fds, std::span<IoEvent> out) const noexcept {
    int produced = 0;

    for (const auto& [handle, entry] : entries_) {
      if (produced >= static_cast<int>(out.size())) break;

      const auto flags = GetTriggeredFlags(fds, handle);

      if (flags == IoEventFlags::kNone) continue;

      out[produced++] = IoEvent{handle, flags, entry.user_data};
    }

    return produced;
  }

  int SelectImpl(std::span<IoEvent> out, std::optional<std::chrono::nanoseconds> timeout) override {
    FdSets fds;
    fds.AddRead(wakeup_pipe_.ReadHandle());
    NativeHandle max_handle = wakeup_pipe_.ReadHandle();
    PopulateFdSets(fds, max_handle);

    timeval tv{};
    timeval* tv_ptr = BuildTimeout(timeout, tv);
    int nready = CallSelect(fds, max_handle, tv_ptr);

    if (nready == 0) return 0;

    if (fds.IsReadable(wakeup_pipe_.ReadHandle())) wakeup_pipe_.Drain();

    return CollectEvents(fds, out);
  }

  EntryMap entries_;
  WakeupPipe wakeup_pipe_;
};

}  // namespace asyncio
