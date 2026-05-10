#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "asyncio/config.hh"

#if defined(ASYNCIO_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>  // SOCKET, INVALID_SOCKET
#include <ws2tcpip.h>
using NativeHandle = SOCKET;
inline constexpr NativeHandle kInvalidHandle = INVALID_SOCKET;
#else
using NativeHandle = int;
inline constexpr NativeHandle kInvalidHandle = -1;
#endif

namespace asyncio {

/// \brief Bitmask flags that describe I/O readiness conditions and registration
///        options.
///
/// These flags are used both:
/// - In `IoEvent::events` to report which conditions occurred on a handle.
/// - In `Register()` to specify which conditions to monitor and which
///   notification behaviors to enable.
///
/// Multiple flags may be combined with bitwise OR (`|`).
///
/// Event condition flags:
/// - `kReadable` : Data is available to read without blocking.
/// - `kWritable` : Data can be written without blocking.
/// - `kError`    : An error condition occurred on the handle.
/// - `kHangup`   : The peer closed the connection or the handle was
///                 disconnected.
///
/// Registration option flags:
/// - `kEdge`     : Use edge-triggered notifications. Events are delivered only
///                 when the readiness state changes.
/// - `kOneShot`  : Deliver at most one notification. The handle must be
///                 re-registered to receive subsequent events.
///
/// `kNone` indicates that no flags are set.
enum class IoEventFlags : uint32_t {
  kNone = 0u,
  kReadable = 1u << 0,
  kWritable = 1u << 1,
  kError = 1u << 2,
  kHangup = 1u << 3,
  kEdge = 1u << 4,
  kOneShot = 1u << 5,
  kMask = kReadable | kWritable | kError | kHangup | kEdge | kOneShot
};

inline constexpr IoEventFlags operator|(IoEventFlags a, IoEventFlags b) noexcept {
  return static_cast<IoEventFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr IoEventFlags operator&(IoEventFlags a, IoEventFlags b) noexcept {
  return static_cast<IoEventFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline constexpr IoEventFlags operator~(IoEventFlags a) noexcept {
  return static_cast<IoEventFlags>(~static_cast<uint32_t>(a));
}

inline IoEventFlags& operator|=(IoEventFlags& a, IoEventFlags b) noexcept { return a = a | b; }

inline IoEventFlags& operator&=(IoEventFlags& a, IoEventFlags b) noexcept { return a = a & b; }

/// \brief Converts a set of `IoEventFlags` to a human-readable string.
///
/// The returned string contains the names of all flags present in `flags`,
/// separated by `'|'`.
///
/// Examples:
/// \code
/// IoEventFlagsToString(kNone);                    // "None"
/// IoEventFlagsToString(kReadable);                // "Readable"
/// IoEventFlagsToString(kReadable | kWritable);    // "Readable|Writable"
/// IoEventFlagsToString(kError | kHangup);         // "Error|Hangup"
/// \endcode
///
/// Unknown bits that do not correspond to any defined `IoEventFlags` are
/// appended as a hexadecimal value (for example, `"0x40"`).
[[nodiscard]] inline std::string IoEventFlagsToString(IoEventFlags flags) {
  if (flags == IoEventFlags::kNone) return "None";

  std::string result;
  auto append = [&result](const char* name) {
    if (!result.empty()) result += '|';

    result += name;
  };

  if ((flags & IoEventFlags::kReadable) != IoEventFlags::kNone) {
    append("Readable");
  }

  if ((flags & IoEventFlags::kWritable) != IoEventFlags::kNone) {
    append("Writable");
  }

  if ((flags & IoEventFlags::kError) != IoEventFlags::kNone) {
    append("Error");
  }

  if ((flags & IoEventFlags::kHangup) != IoEventFlags::kNone) {
    append("Hangup");
  }

  if ((flags & IoEventFlags::kEdge) != IoEventFlags::kNone) {
    append("Edge");
  }

  if ((flags & IoEventFlags::kOneShot) != IoEventFlags::kNone) {
    append("OneShot");
  }

  const uint32_t unknown = static_cast<uint32_t>(flags & (~IoEventFlags::kMask));

  if (unknown != 0u) {
    if (!result.empty()) result += '|';

    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%X", unknown);
    result += buffer;
  }

  return result;
}

/// \brief Describes a single readiness notification returned by the I/O
///        poller.
///
/// Each `IoEvent` corresponds to one registered handle whose state changed.
/// The `events` field contains one or more `IoEventFlags` indicating which
/// readiness or error conditions were observed. The `user_data` field is the
/// same opaque pointer supplied when the handle was registered, allowing the
/// caller to associate application-specific state with the event.
///
/// Typical usage:
/// \code
/// IoEvent event = ...;
/// if (event.IsReadable()) {
///   // Read available data.
/// }
/// if (event.IsWritable()) {
///   // Write pending data.
/// }
/// if (event.IsError() || event.IsHungUp()) {
///   // Handle disconnection or failure.
/// }
/// \endcode
struct IoEvent {
  /// The handle for which the notification was generated.
  NativeHandle handle{kInvalidHandle};

  /// Bitmask of readiness and error conditions.
  IoEventFlags events{IoEventFlags::kNone};

  /// Opaque pointer supplied during `Register()`.
  void* user_data{nullptr};

  /// \brief Returns `true` if the handle is ready for reading without
  ///        blocking.
  [[nodiscard]] bool IsReadable() const noexcept {
    return (events & IoEventFlags::kReadable) != IoEventFlags::kNone;
  }

  /// \brief Returns `true` if the handle is ready for writing without
  ///        blocking.
  [[nodiscard]] bool IsWritable() const noexcept {
    return (events & IoEventFlags::kWritable) != IoEventFlags::kNone;
  }

  /// \brief Returns `true` if an error condition was reported for the handle.
  [[nodiscard]] bool IsError() const noexcept {
    return (events & IoEventFlags::kError) != IoEventFlags::kNone;
  }

  /// \brief Returns `true` if the peer closed the connection or the handle
  ///       was disconnected.
  [[nodiscard]] bool IsHungUp() const noexcept {
    return (events & IoEventFlags::kHangup) != IoEventFlags::kNone;
  }
};

/// \brief Describes the features and limits supported by an I/O selector.
///
/// This structure reports which notification modes and auxiliary features are
/// available in the underlying polling backend (for example, `epoll`,
/// `kqueue`, `poll`, or `select`).
///
/// Capability flags indicate which notification modes and auxiliary features
/// are available:
/// - `edge_triggered` : Supports edge-triggered notifications (`kEdge`).
/// - `one_shot`       : Supports one-shot notifications (`kOneShot`).
/// - `level_triggered`: Supports traditional level-triggered notifications.
/// - `wakeup`         : Supports waking a blocked `Wait()` call from another
///                      thread (for example via `WakeUp()`).
/// - `proactive`      : Supports proactive, not reactive, event delivery (for
///                      example, IOCP's 0-byte overlapped trick or io_uring's
///                      proactive completion queue).  Proactive backends may
///                      report events immediately when registration is
///                      performed, without waiting for a state change.  This
///                      can reduce latency and improve responsiveness.
///
/// `max_handles` specifies the maximum number of handles that can be
/// registered simultaneously. A value of `0` = OS-defined (effectively
/// unlimited).
///
/// Typical usage:
/// \code
/// SelectorCapabilities caps = selector.GetCapabilities();
///
/// if (caps.edge_triggered) {
///   // Prefer edge-triggered registration for better scalability.
/// }
///
/// if (caps.max_handles != 0) {
///   std::cout << "Supports up to " << caps.max_handles << " handles\n";
/// }
/// \endcode
struct SelectorCapabilities {
  /// Supports edge-triggered notifications (`kEdge`).
  bool edge_triggered{false};

  /// Supports one-shot notifications (`kOneShot`).
  bool one_shot{false};

  /// Supports level-triggered notifications.
  bool level_triggered{true};

  /// Supports waking a blocked `Wait()` call.
  bool wakeup{false};

  /// Supports proactive event delivery (e.g., io_uring and IOCP can report
  /// events proactively on registration, without waiting for a state change.
  bool proactive{false};

  ///< Maximum number of simultaneously registered handles; `0` = OS-defined
  ///< (effectively unlimited).
  size_t max_handles{0};
};

/// \brief Abstract interface for scalable I/O event demultiplexing.
///
/// `Selector` provides a unified interface over operating-system specific
/// readiness notification mechanisms such as:
/// - `epoll` on Linux,
/// - `kqueue` on BSD and macOS,
/// - `poll` / `select` as portable fallbacks,
/// - `WSAPoll` or I/O completion mechanisms on Windows.
///
/// Callers register native handles together with an `IoEventFlags` interest
/// mask and an optional `user_data` pointer. The selector blocks in `Select()`
/// until one or more handles become ready, then writes `IoEvent` records into a
/// caller-supplied buffer.
///
/// The API is designed for high-performance event loops:
/// - No heap allocations occur on the hot path.
/// - The caller owns and reuses the event buffer.
/// - Arbitrary application state can be attached through `user_data`.
/// - Optional cross-thread wakeup is provided through `Interrupt()`.
///
/// Thread safety:
/// - Concurrent calls to `Register()`, `Modify()`, `Unregister()`, and
///   `ModifyUserData()` are implementation-defined unless stated otherwise by a
///   concrete backend.
/// - `Interrupt()` is intended to be safe to call from other threads.
/// - `Select()` should not be called concurrently from multiple threads unless
///   explicitly supported by the implementation.
///
/// Typical usage:
/// \code
/// std::unique_ptr<Selector> selector = CreateBestSelector();
///
/// selector->Register(socket, kReadable, connection);
///
/// std::array<IoEvent, 128> events;
/// while (running) {
///   int n = selector->Select(events, std::chrono::milliseconds{100});
///   for (const IoEvent& ev : std::span{events}.first(n)) {
///     auto* conn = static_cast<Connection*>(ev.user_data);
///     if (ev.IsReadable()) {
///       conn->OnReadable();
///     }
///     if (ev.IsError() || ev.IsHungUp()) {
///       conn->Close();
///     }
///   }
/// }
/// \endcode
///
/// Object lifetime:
/// - `Selector` is non-copyable and non-movable because implementations manage
///   operating-system resources such as file descriptors, handles, and wakeup
///   primitives.
/// - Destroying the selector automatically releases all associated resources.
class Selector {
 public:
  virtual ~Selector() = default;

  // Non-copyable, non-movable.
  Selector(const Selector&) = delete;
  Selector& operator=(const Selector&) = delete;
  Selector(Selector&&) = delete;
  Selector& operator=(Selector&&) = delete;

  /// \brief Register a handle for monitoring.
  ///
  /// \param handle    Platform handle to monitor (socket/fd).
  /// \param events    Bitmask of IoEventFlags (kReadable | kWritable | ...).
  /// \param user_data Opaque pointer stored in every IoEvent for this handle.
  /// \throws          std::invalid_argument if handle == kInvalidHandle.
  /// \throws          std::invalid_argument if handle is already registered.
  /// \throws          std::system_error     on OS failure.
  virtual void Register(NativeHandle handle, IoEventFlags events, void* user_data = nullptr) = 0;

  /// \brief Modify the event mask for an already-registered handle.
  ///
  /// \param handle Platform handle already registered.
  /// \param events New bitmask of IoEventFlags.
  /// \throws       std::invalid_argument if handle is not currently registered.
  /// \throws       std::system_error     on OS failure.
  virtual void Modify(NativeHandle handle, IoEventFlags events) = 0;

  /// \brief Modify user_data for an already-registered handle (no OS syscall
  ///        needed).
  ///
  /// \throws std::invalid_argument if handle is not currently registered.
  virtual void ModifyUserData(NativeHandle handle, void* user_data) = 0;

  /// \brief Remove a handle from monitoring.
  ///
  /// No-op if the handle was not registered.
  ///
  /// \throws std::system_error on OS failure.
  virtual void Unregister(NativeHandle handle) = 0;

  /// \brief Get the number of currently registered handles.
  ///
  /// \return Number of registered handles.
  [[nodiscard]] virtual size_t Count() const noexcept = 0;

  /// \brief Interrupt a Select() call that is currently blocking in another
  ///        thread (or that will block in the future) and cause it to return 0
  ///        promptly.
  ///
  /// The interrupted Select() drains the wakeup token internally; no spurious
  /// events appear in the output span.  Multiple concurrent calls to
  /// Interrupt() are coalesced — only one Select() wakeup is guaranteed.
  ///
  /// Thread-safe: may be called from any thread, including signal handlers
  /// (on POSIX backends backed by eventfd or a pipe).
  ///
  /// \throws std::logic_error  if Capabilities().wakeup == false.
  /// \throws std::system_error on OS failure (write to eventfd/pipe).
  virtual void Interrupt() = 0;

  /// \brief Waits for I/O events for up to the specified duration.
  ///
  /// This overload accepts any `std::chrono::duration` type (for example
  /// `10ms`, `250us`, or `1s`) and converts it internally to nanosecond
  /// resolution before dispatching to the backend implementation.
  ///
  /// Timeout semantics:
  /// - `timeout > 0` : Block for at most the specified duration.
  /// - `timeout == 0`: Poll without blocking and return immediately.
  /// - `timeout < 0` : Undefined behavior; callers should provide only
  ///                   non-negative durations.
  ///
  /// \param out     Destination buffer that will be filled with ready I/O events. The number of
  ///                events written will not exceed `out.size()`.
  /// \param timeout Maximum amount of time to wait for events.
  /// \return        Number of events written to `out`, in the range `[0, out.size()]`.
  /// \throws        std::system_error If the underlying operating-system call
  ///                fails.
  template <class Rep, class Period>
  int Select(std::span<IoEvent> out, std::chrono::duration<Rep, Period> timeout) {
    if (timeout < timeout.zero()) {
      throw std::invalid_argument("Selector::Select timeout must be non-negative");
    }

    return SelectImpl(out, std::chrono::duration_cast<std::chrono::nanoseconds>(timeout));
  }

  /// \brief Waits indefinitely until at least one I/O event becomes ready.
  ///
  /// This overload performs an unbounded wait and is equivalent to calling
  /// `SelectImpl(out, std::nullopt)`.
  ///
  /// \param out Destination buffer that will be filled with ready I/O events. The number of events
  ///            written will not exceed `out.size()`.
  /// \return    Number of events written to `out`, in the range `[0, out.size()]`.
  /// \throws    std::system_error If the underlying operating-system call fails.
  int Select(std::span<IoEvent> out) { return SelectImpl(out, std::nullopt); }

  /// \brief Human-readable name of the backend implementation.
  ///
  /// Examples include "epoll", "kqueue", and "WSAPoll".
  [[nodiscard]] virtual const char* BackendName() const noexcept = 0;

  /// \brief Returns the feature flags supported by this backend.
  ///
  /// \return A `SelectorCapabilities` structure describing the features and limits of this selector
  ///         implementation.
  [[nodiscard]] virtual SelectorCapabilities Capabilities() const noexcept = 0;

 protected:
  Selector() = default;

  /// \brief Backend-specific implementation of event selection.
  ///
  /// \param out     Output buffer for ready events.
  /// \param timeout Maximum wait duration in nanoseconds, or `std::nullopt` to wait indefinitely.
  /// \return        Number of events written to `out`.
  /// \throws        std::system_error If the underlying operating-system call fails.
  virtual int SelectImpl(std::span<IoEvent> out,
                         std::optional<std::chrono::nanoseconds> timeout) = 0;
};

[[nodiscard]] std::unique_ptr<Selector> CreateSelector(size_t initial_capacity = 64);

}  // namespace asyncio
