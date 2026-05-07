// Copyright 2025 asyncio-cpp authors. All rights reserved.

#include "asyncio/sleep.h"

#include "asyncio/event_loop.h"

namespace asyncio {

Future<void> Sleep(std::chrono::nanoseconds duration) {
  auto* loop = EventLoop::Current();
  Future<void> future;
  // The lambda captures the Future by value (copy). Both the copy and the
  // returned Future share the same internal state. When the timer fires,
  // resolving the copy also resolves the returned Future.
  loop->CallLater(duration, [f = future]() mutable {
    if (!f.Done()) {
      f.SetResult();
    }
  });
  return future;
}

Future<void> Yield() {
  auto* loop = EventLoop::Current();
  Future<void> future;
  loop->CallSoon([f = future]() mutable {
    if (!f.Done()) {
      f.SetResult();
    }
  });
  return future;
}

}  // namespace asyncio
