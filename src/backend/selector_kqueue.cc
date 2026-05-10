// Copyright 2025 asyncio-cpp authors. All rights reserved.

#include "asyncio/detail/selector_kqueue.h"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace asyncio::detail {

KqueueSelector::KqueueSelector() {
  kqueue_fd_ = kqueue();
  if (kqueue_fd_ < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "KqueueSelector: kqueue() failed");
  }
}

KqueueSelector::~KqueueSelector() {
  if (kqueue_fd_ >= 0) {
    close(kqueue_fd_);
  }
}

void KqueueSelector::ApplyChanges(struct kevent* changes, int nchanges) {
  int ret = kevent(kqueue_fd_, changes, nchanges, nullptr, 0, nullptr);
  if (ret < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "KqueueSelector: kevent() changelist failed");
  }
}

void KqueueSelector::Register(int fd, uint32_t events) {
  struct kevent changes[2];
  int nchanges = 0;

  if (events & kReadable) {
    EV_SET(&changes[nchanges++], static_cast<uintptr_t>(fd), EVFILT_READ,
           EV_ADD | EV_ENABLE, 0, 0, nullptr);
  }
  if (events & kWritable) {
    EV_SET(&changes[nchanges++], static_cast<uintptr_t>(fd), EVFILT_WRITE,
           EV_ADD | EV_ENABLE, 0, 0, nullptr);
  }

  if (nchanges > 0) {
    ApplyChanges(changes, nchanges);
  }
  registered_[fd] = events;
}

void KqueueSelector::Modify(int fd, uint32_t new_events) {
  auto it = registered_.find(fd);
  if (it == registered_.end()) {
    throw std::invalid_argument(
        "KqueueSelector::Modify: fd not registered");
  }

  uint32_t old_events = it->second;
  struct kevent changes[4];
  int nchanges = 0;

  // Add new event filters.
  if ((new_events & kReadable) && !(old_events & kReadable)) {
    EV_SET(&changes[nchanges++], static_cast<uintptr_t>(fd), EVFILT_READ,
           EV_ADD | EV_ENABLE, 0, 0, nullptr);
  }
  if ((new_events & kWritable) && !(old_events & kWritable)) {
    EV_SET(&changes[nchanges++], static_cast<uintptr_t>(fd), EVFILT_WRITE,
           EV_ADD | EV_ENABLE, 0, 0, nullptr);
  }

  // Remove old event filters no longer needed.
  if (!(new_events & kReadable) && (old_events & kReadable)) {
    EV_SET(&changes[nchanges++], static_cast<uintptr_t>(fd), EVFILT_READ,
           EV_DELETE, 0, 0, nullptr);
  }
  if (!(new_events & kWritable) && (old_events & kWritable)) {
    EV_SET(&changes[nchanges++], static_cast<uintptr_t>(fd), EVFILT_WRITE,
           EV_DELETE, 0, 0, nullptr);
  }

  if (nchanges > 0) {
    ApplyChanges(changes, nchanges);
  }
  it->second = new_events;
}

void KqueueSelector::Unregister(int fd) {
  auto it = registered_.find(fd);
  if (it == registered_.end()) return;  // No-op if not registered.

  uint32_t events = it->second;
  struct kevent changes[2];
  int nchanges = 0;

  if (events & kReadable) {
    EV_SET(&changes[nchanges++], static_cast<uintptr_t>(fd), EVFILT_READ,
           EV_DELETE, 0, 0, nullptr);
  }
  if (events & kWritable) {
    EV_SET(&changes[nchanges++], static_cast<uintptr_t>(fd), EVFILT_WRITE,
           EV_DELETE, 0, 0, nullptr);
  }

  // Best-effort delete — fd may already be closed.
  if (nchanges > 0) {
    kevent(kqueue_fd_, changes, nchanges, nullptr, 0, nullptr);
  }
  registered_.erase(it);
}

std::vector<IoEvent> KqueueSelector::Select(
    std::optional<std::chrono::nanoseconds> timeout) {
  // Build the timespec, if any.
  struct timespec ts{};
  struct timespec* ts_ptr = nullptr;

  if (timeout.has_value()) {
    auto ns = timeout->count();
    if (ns < 0) ns = 0;
    ts.tv_sec = static_cast<time_t>(ns / 1'000'000'000LL);
    ts.tv_nsec = static_cast<long>(ns % 1'000'000'000LL);
    ts_ptr = &ts;
  }
  // ts_ptr == nullptr → block indefinitely.

  constexpr int kMaxEvents = 64;
  struct kevent events[kMaxEvents];

  int nready =
      kevent(kqueue_fd_, nullptr, 0, events, kMaxEvents, ts_ptr);

  if (nready < 0) {
    if (errno == EINTR) return {};  // Interrupted by a signal — not an error.
    throw std::system_error(errno, std::generic_category(),
                            "KqueueSelector: kevent() wait failed");
  }

  // Merge events per fd (kqueue may return separate events for READ / WRITE).
  std::vector<IoEvent> result;
  result.reserve(static_cast<size_t>(nready));

  for (int i = 0; i < nready; ++i) {
    const struct kevent& ev = events[i];
    int fd = static_cast<int>(ev.ident);

    // Find if we already have an entry for this fd.
    bool found = false;
    for (auto& io : result) {
      if (io.fd == fd) {
        if (ev.filter == EVFILT_READ) io.events |= kReadable;
        if (ev.filter == EVFILT_WRITE) io.events |= kWritable;
        found = true;
        break;
      }
    }
    if (!found) {
      uint32_t flags = 0;
      if (ev.filter == EVFILT_READ) flags |= kReadable;
      if (ev.filter == EVFILT_WRITE) flags |= kWritable;
      result.push_back({fd, flags});
    }
  }

  return result;
}

}  // namespace asyncio::detail

#endif  // __APPLE__ / BSD
