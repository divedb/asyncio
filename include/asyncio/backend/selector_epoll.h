// Copyright 2025 asyncio-cpp authors. All rights reserved.
// EpollSelector — Linux epoll-based I/O multiplexing backend.

#ifndef ASYNCIO_DETAIL_SELECTOR_EPOLL_H_
#define ASYNCIO_DETAIL_SELECTOR_EPOLL_H_

#include "asyncio/detail/selector.h"

#if defined(__linux__)

#include <unordered_map>

namespace asyncio::detail {

/// Selector implementation backed by Linux epoll.
///
/// Uses EPOLLIN / EPOLLOUT flags via epoll_create1() / epoll_ctl() /
/// epoll_wait().
class EpollSelector final : public Selector {
 public:
  EpollSelector();
  ~EpollSelector() override;

  EpollSelector(const EpollSelector&) = delete;
  EpollSelector& operator=(const EpollSelector&) = delete;
  EpollSelector(EpollSelector&&) = delete;
  EpollSelector& operator=(EpollSelector&&) = delete;

  void Register(int fd, uint32_t events) override;
  void Modify(int fd, uint32_t events) override;
  void Unregister(int fd) override;
  std::vector<IoEvent> Select(
      std::optional<std::chrono::nanoseconds> timeout) override;

 private:
  int epoll_fd_ = -1;

  /// Tracks registered fds for Unregister without EPOLL_CTL_DEL needing data.
  std::unordered_map<int, uint32_t> registered_;

  /// Converts IoEventFlags to EPOLL flags.
  static uint32_t ToEpollEvents(uint32_t io_events);
};

}  // namespace asyncio::detail

#endif  // __linux__

#endif  // ASYNCIO_DETAIL_SELECTOR_EPOLL_H_
