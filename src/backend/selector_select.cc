#include "asyncio/backend/selector_select.hh"

#include <sys/select.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace asyncio::detail {

void SelectSelector::Register(NativeHandle handle, IoEventFlags events, void* user_data) {}

void SelectSelector::Register(int fd, uint32_t events) {
  if (fd < 0 || fd >= FD_SETSIZE) {
    throw std::invalid_argument("SelectSelector::Register: fd out of range for select()");
  }
  registered_[fd] = events;
}

void SelectSelector::Modify(int fd, uint32_t new_events) {
  if (registered_.find(fd) == registered_.end()) {
    throw std::invalid_argument("SelectSelector::Modify: fd not registered");
  }
  registered_[fd] = new_events;
}

void SelectSelector::Unregister(int fd) { registered_.erase(fd); }

std::vector<IoEvent> SelectSelector::Select(std::optional<std::chrono::nanoseconds> timeout) {
  fd_set read_fds;
  fd_set write_fds;
  FD_ZERO(&read_fds);
  FD_ZERO(&write_fds);

  int max_fd = -1;

  for (const auto& [fd, events] : registered_) {
    if (events & kReadable) FD_SET(fd, &read_fds);
    if (events & kWritable) FD_SET(fd, &write_fds);
    if (fd > max_fd) max_fd = fd;
  }

  struct timeval tv{};
  struct timeval* tv_ptr = nullptr;

  if (timeout.has_value()) {
    auto ns = timeout->count();
    if (ns < 0) ns = 0;
    tv.tv_sec = static_cast<time_t>(ns / 1'000'000'000LL);
    tv.tv_usec = static_cast<suseconds_t>((ns % 1'000'000'000LL) / 1000LL);
    tv_ptr = &tv;
  }
  // tv_ptr == nullptr → block indefinitely.

  int nready = select(max_fd + 1, &read_fds, &write_fds, nullptr, tv_ptr);

  if (nready < 0) {
    if (errno == EINTR) return {};
    throw std::system_error(errno, std::generic_category(), "SelectSelector: select() failed");
  }

  std::vector<IoEvent> result;
  result.reserve(static_cast<size_t>(nready));

  for (const auto& [fd, events] : registered_) {
    uint32_t ready = 0;
    if ((events & kReadable) && FD_ISSET(fd, &read_fds)) ready |= kReadable;
    if ((events & kWritable) && FD_ISSET(fd, &write_fds)) ready |= kWritable;
    if (ready != 0) {
      result.push_back({fd, ready});
    }
  }

  return result;
}

}  // namespace asyncio::detail
