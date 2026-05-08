// Copyright 2025 asyncio-cpp authors. All rights reserved.

#include "asyncio/detail/selector_epoll.h"

#if defined(__linux__)

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace asyncio::detail {

// static
uint32_t EpollSelector::ToEpollEvents(uint32_t io_events) {
  uint32_t epoll_events = 0;
  if (io_events & kReadable) epoll_events |= EPOLLIN;
  if (io_events & kWritable) epoll_events |= EPOLLOUT;
  return epoll_events;
}

EpollSelector::EpollSelector() {
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "EpollSelector: epoll_create1() failed");
  }
}

EpollSelector::~EpollSelector() {
  if (epoll_fd_ >= 0) {
    close(epoll_fd_);
  }
}

void EpollSelector::Register(int fd, uint32_t events) {
  struct epoll_event ev{};
  ev.events = ToEpollEvents(events);
  ev.data.fd = fd;

  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "EpollSelector: epoll_ctl EPOLL_CTL_ADD failed");
  }
  registered_[fd] = events;
}

void EpollSelector::Modify(int fd, uint32_t new_events) {
  if (registered_.find(fd) == registered_.end()) {
    throw std::invalid_argument(
        "EpollSelector::Modify: fd not registered");
  }

  struct epoll_event ev{};
  ev.events = ToEpollEvents(new_events);
  ev.data.fd = fd;

  if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "EpollSelector: epoll_ctl EPOLL_CTL_MOD failed");
  }
  registered_[fd] = new_events;
}

void EpollSelector::Unregister(int fd) {
  auto it = registered_.find(fd);
  if (it == registered_.end()) return;  // No-op.

  // epoll_ctl with EPOLL_CTL_DEL ignores the event struct (Linux 2.6.9+).
  epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  registered_.erase(it);
}

std::vector<IoEvent> EpollSelector::Select(
    std::optional<std::chrono::nanoseconds> timeout) {
  int timeout_ms = -1;  // -1 means block indefinitely.
  if (timeout.has_value()) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(*timeout);
    timeout_ms = static_cast<int>(ms.count());
    if (timeout_ms < 0) timeout_ms = 0;
  }

  constexpr int kMaxEvents = 64;
  struct epoll_event events[kMaxEvents];

  int nready = epoll_wait(epoll_fd_, events, kMaxEvents, timeout_ms);
  if (nready < 0) {
    if (errno == EINTR) return {};
    throw std::system_error(errno, std::generic_category(),
                            "EpollSelector: epoll_wait() failed");
  }

  std::vector<IoEvent> result;
  result.reserve(static_cast<size_t>(nready));

  for (int i = 0; i < nready; ++i) {
    const struct epoll_event& ev = events[i];
    uint32_t flags = 0;
    if (ev.events & (EPOLLIN | EPOLLHUP | EPOLLERR)) flags |= kReadable;
    if (ev.events & (EPOLLOUT | EPOLLERR)) flags |= kWritable;
    result.push_back({ev.data.fd, flags});
  }

  return result;
}

}  // namespace asyncio::detail

#endif  // __linux__
