// Copyright 2025 asyncio-cpp authors. All rights reserved.
// KqueueSelector — macOS/BSD kqueue-based I/O multiplexing backend.

#ifndef ASYNCIO_DETAIL_SELECTOR_KQUEUE_H_
#define ASYNCIO_DETAIL_SELECTOR_KQUEUE_H_

#include "asyncio/detail/selector.h"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)

#include <sys/event.h>
#include <unordered_map>

namespace asyncio::detail {

/// Selector implementation backed by kqueue (macOS / BSD).
///
/// Uses EVFILT_READ and EVFILT_WRITE filters to monitor file descriptors.
/// Batch-registers changes via kevent() with a non-null changelist.
class KqueueSelector final : public Selector {
 public:
  KqueueSelector();
  ~KqueueSelector() override;

  KqueueSelector(const KqueueSelector&) = delete;
  KqueueSelector& operator=(const KqueueSelector&) = delete;
  KqueueSelector(KqueueSelector&&) = delete;
  KqueueSelector& operator=(KqueueSelector&&) = delete;

  void Register(int fd, uint32_t events) override;
  void Modify(int fd, uint32_t events) override;
  void Unregister(int fd) override;
  std::vector<IoEvent> Select(
      std::optional<std::chrono::nanoseconds> timeout) override;

 private:
  int kqueue_fd_ = -1;

  /// Tracks which events are currently registered per fd.
  std::unordered_map<int, uint32_t> registered_;

  /// Applies a set of kevent changes to the kqueue.
  void ApplyChanges(struct kevent* changes, int nchanges);
};

}  // namespace asyncio::detail

#endif  // __APPLE__ / BSD

#endif  // ASYNCIO_DETAIL_SELECTOR_KQUEUE_H_
