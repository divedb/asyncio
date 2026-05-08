// Copyright 2025 asyncio-cpp authors. All rights reserved.
// SelectSelector — portable select()-based I/O multiplexing backend.

#ifndef ASYNCIO_DETAIL_SELECTOR_SELECT_H_
#define ASYNCIO_DETAIL_SELECTOR_SELECT_H_

#include "asyncio/detail/selector.h"

#include <unordered_map>

namespace asyncio::detail {

/// Selector implementation backed by POSIX select().
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

  SelectSelector(const SelectSelector&) = delete;
  SelectSelector& operator=(const SelectSelector&) = delete;
  SelectSelector(SelectSelector&&) = delete;
  SelectSelector& operator=(SelectSelector&&) = delete;

  void Register(int fd, uint32_t events) override;
  void Modify(int fd, uint32_t events) override;
  void Unregister(int fd) override;
  std::vector<IoEvent> Select(
      std::optional<std::chrono::nanoseconds> timeout) override;

 private:
  /// Tracks event flags per fd.
  std::unordered_map<int, uint32_t> registered_;
};

}  // namespace asyncio::detail

#endif  // ASYNCIO_DETAIL_SELECTOR_SELECT_H_
