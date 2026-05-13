#pragma once

#include "asyncio/backend/selector.hh"

#if defined(ASYNCIO_OS_LINUX)

#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <system_error>
#include <unordered_map>

#include "asyncio/backend/wakeup_pipe.hh"

namespace asyncio {

[[nodiscard]] inline uint32_t ToEpollEvents(IoEventFlags flags) noexcept {
  uint32_t ev = 0;

  if ((flags & IoEventFlags::kReadable) != IoEventFlags::kNone) ev |= EPOLLIN;
  if ((flags & IoEventFlags::kWritable) != IoEventFlags::kNone) ev |= EPOLLOUT;
  if ((flags & IoEventFlags::kEdge) != IoEventFlags::kNone) ev |= EPOLLET;
  if ((flags & IoEventFlags::kOneShot) != IoEventFlags::kNone) ev |= EPOLLONESHOT;

  return ev;
}

[[nodiscard]] inline IoEventFlags FromEpollEvents(uint32_t ev) noexcept {
  IoEventFlags flags = IoEventFlags::kNone;

  if (ev & (EPOLLIN | EPOLLPRI)) flags |= IoEventFlags::kReadable;
  if (ev & EPOLLOUT) flags |= IoEventFlags::kWritable;
  if (ev & EPOLLERR) flags |= IoEventFlags::kError;
  if (ev & EPOLLHUP) flags |= IoEventFlags::kHangup;
#ifdef EPOLLRDHUP
  if (ev & EPOLLRDHUP) flags |= IoEventFlags::kHangup;
#endif

  return flags;
}

/// \brief Converts an optional nanosecond duration to the millisecond integer
///        expected by epoll_wait.  Returns -1 to indicate "block indefinitely".
///
/// \param timeout The optional timeout duration in nanoseconds.
/// \return        The timeout in milliseconds, or -1 to block indefinitely.
[[nodiscard]] inline int ToTimeoutMs(std::optional<std::chrono::nanoseconds> timeout) noexcept {
  if (!timeout.has_value()) return -1;

  const auto clamped = std::max(*timeout, std::chrono::nanoseconds::zero());
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clamped);

  // A positive sub-millisecond timeout must not be rounded to 0 (that would
  // make epoll_wait return immediately without waiting).
  if (clamped > std::chrono::nanoseconds::zero() && ms.count() == 0) return 1;

  constexpr int64_t kIntMax = std::numeric_limits<int>::max();

  return (ms.count() > kIntMax) ? static_cast<int>(kIntMax) : static_cast<int>(ms.count());
}

/// \brief Linux-specific selector implementation based on `epoll`.
///
/// `EpollSelector` is a concrete implementation of `Selector` that uses the
/// Linux kernel's :contentReference[oaicite:0]{index=0} API to efficiently monitor a
/// large number of file descriptors.
///
/// Key characteristics:
/// - Uses `epoll_create1(EPOLL_CLOEXEC)` to create the epoll instance.
/// - Supports scalable I/O multiplexing with near O(1) readiness notification.
/// - Suitable for high-concurrency event loops and Reactor-style architectures.
/// - Typically used with non-blocking sockets so that event handlers never block
///   the event loop thread.
///
/// Constructor behavior:
/// - Creates the underlying epoll file descriptor.
/// - Sets the close-on-exec flag (`EPOLL_CLOEXEC`) to prevent descriptor leaks
///   across `exec()` calls.
/// - Throws `std::system_error` if epoll instance creation fails.
///
/// Internal wake-up mechanism:
/// - The constructor also initializes a dedicated wake-up event (for example via
///   `eventfd`) and registers it with epoll.
/// - This allows other threads to interrupt a blocking `Wait()` call so the
///   selector can process newly queued tasks, timer updates, or shutdown
///   requests.
///
/// Threading model:
/// - One thread typically owns the selector and repeatedly calls `Wait()`.
/// - Other threads may safely signal the wake-up event to notify the event loop.
///
/// Platform requirements:
/// - Available only on Linux systems that support the epoll API.
///
/// Performance notes:
/// - `epoll` is edge- or level-triggered and avoids the O(n) descriptor scans
///   associated with `select()` and `poll()`, making it the standard mechanism
///   for high-performance network servers such as :contentReference[oaicite:1]{index=1} and
///   :contentReference[oaicite:2]{index=2}.
///
/// \brief Linux epoll-based implementation of `Selector`.
class EpollSelector final : public Selector {
 public:
  EpollSelector() {
    CreateEpollFd();
    RegisterWakeupPipe();
  }

  ~EpollSelector() override {
    if (epoll_fd_ >= 0) CloseHandle(epoll_fd_);
  }

  void Register(NativeHandle handle, IoEventFlags events, void* user_data) override {
    CheckHandle(handle, "Register");

    auto [it, inserted] = entries_.try_emplace(handle, events, user_data);

    if (!inserted) {
      throw std::invalid_argument("EpollSelector::Register: handle already registered");
    }

    epoll_event ev = MakeEpollEvent(handle, events);

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, handle, &ev) != 0) {
      const int err = errno;
      entries_.erase(it);

      throw std::system_error(err, std::system_category(),
                              "EpollSelector::Register epoll_ctl add failed");
    }
  }

  void Modify(NativeHandle handle, IoEventFlags events) override {
    auto it = RequireRegistered(handle, "Modify");
    epoll_event ev = MakeEpollEvent(handle, events);

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, handle, &ev) != 0) {
      throw std::system_error(errno, std::system_category(),
                              "EpollSelector::Modify epoll_ctl mod failed");
    }

    it->second.events = events;
  }

  void ModifyUserData(NativeHandle handle, void* user_data) override {
    auto it = RequireRegistered(handle, "ModifyUserData");
    it->second.user_data = user_data;
  }

  void Unregister(NativeHandle handle) override {
    auto it = entries_.find(handle);

    if (it == entries_.end()) return;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, handle, nullptr) != 0) {
      throw std::system_error(errno, std::system_category(),
                              "EpollSelector::Unregister epoll_ctl del failed");
    }

    entries_.erase(it);
  }

  [[nodiscard]] size_t Count() const noexcept override { return entries_.size(); }

  void Interrupt() override { wakeup_pipe_.Wakeup(); }

  [[nodiscard]] const char* BackendName() const noexcept override { return "epoll"; }

  [[nodiscard]] SelectorCapabilities Capabilities() const noexcept override {
    return SelectorCapabilities{
        .edge_triggered = true,
        .level_triggered = true,
        .one_shot = true,
        .wakeup = true,
        .proactive = false,
        .max_handles = 0,
    };
  }

 private:
  struct Entry {
    IoEventFlags events{IoEventFlags::kNone};
    void* user_data{nullptr};
  };

  using EntryMap = std::unordered_map<NativeHandle, Entry>;

  EntryMap::iterator RequireRegistered(NativeHandle handle, const char* ctx) {
    auto it = entries_.find(handle);

    if (it == entries_.end()) {
      throw std::invalid_argument(std::string("EpollSelector::") + ctx + ": handle not registered");
    }

    return it;
  }

  /// \brief Waits for I/O events on the epoll file descriptor.
  ///
  /// \param buf        The buffer to store the triggered events.
  /// \param max_events The maximum number of events to retrieve.
  /// \param timeout    The optional timeout duration in nanoseconds.
  /// \return           The number of events retrieved.
  int WaitForEvents(epoll_event* buf, int max_events,
                    std::optional<std::chrono::nanoseconds> timeout) {
    for (;;) {
      const int n = ::epoll_wait(epoll_fd_, buf, max_events, ToTimeoutMs(timeout));

      if (n >= 0) return n;
      if (errno == EINTR) continue;

      throw std::system_error(errno, std::system_category(), "EpollSelector: epoll_wait failed");
    }
  }

  /// \brief Collects I/O events from the raw epoll events.
  /// \param raw The raw epoll events.
  /// \param out The output buffer to store the collected I/O events.
  /// \return The number of events collected.
  int CollectEvents(std::span<const epoll_event> raw, std::span<IoEvent> out) {
    int produced = 0;

    for (const epoll_event& ev : raw) {
      if (ev.data.fd == wakeup_pipe_.ReadHandle()) {
        wakeup_pipe_.Drain();
        continue;
      }

      if (produced >= static_cast<int>(out.size())) break;

      auto it = entries_.find(ev.data.fd);

      if (it == entries_.end()) continue;

      const IoEventFlags flags = FromEpollEvents(ev.events);

      if (flags == IoEventFlags::kNone) continue;

      out[produced++] = IoEvent{ev.data.fd, flags, it->second.user_data};
    }

    return produced;
  }

  int SelectImpl(std::span<IoEvent> out, std::optional<std::chrono::nanoseconds> timeout) override {
    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];
    const int nready = WaitForEvents(events, kMaxEvents, timeout);

    return CollectEvents({events, static_cast<size_t>(nready)}, out);
  }

  /// \brief Creates an epoll file descriptor with the close-on-exec flag set.
  ///
  /// \throws std::system_error if epoll instance creation fails.
  void CreateEpollFd() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);

    if (epoll_fd_ < 0) {
      throw std::system_error(errno, std::system_category(), "EpollSelector: epoll_create1 failed");
    }
  }

  /// \brief Registers the wake-up pipe's read endpoint with the epoll instance.
  void RegisterWakeupPipe() {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = wakeup_pipe_.ReadHandle();

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_pipe_.ReadHandle(), &ev) != 0) {
      const int err = LastError();
      CloseHandle(epoll_fd_);
      epoll_fd_ = -1;

      throw std::system_error(err, std::system_category(),
                              "EpollSelector: failed to register wakeup fd");
    }
  }

  /// \brief Constructs an `epoll_event` structure for the given handle and I/O event flags.
  ///
  /// \param handle The native handle associated with the event.
  /// \param flags  The I/O event flags indicating the conditions to monitor (readable, writable,
  ///               etc.).
  /// \return       An `epoll_event` structure populated with the appropriate event mask and user
  ///               data.
  [[nodiscard]] static epoll_event MakeEpollEvent(NativeHandle handle,
                                                  IoEventFlags flags) noexcept {
    epoll_event ev{};
    ev.events = ToEpollEvents(flags);
    ev.data.fd = handle;

    return ev;
  }

  NativeHandle epoll_fd_{-1};
  EntryMap entries_;
  WakeupPipe wakeup_pipe_;
};

}  // namespace asyncio

#endif  // ASYNCIO_OS_LINUX
