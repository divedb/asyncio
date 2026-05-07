// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Sleep and Yield — timer-based and immediate rescheduling Futures.

#ifndef ASYNCIO_SLEEP_H_
#define ASYNCIO_SLEEP_H_

#include <chrono>

#include "asyncio/future.h"

namespace asyncio {

/// Returns a Future<void> that resolves after the given duration.
/// Must be called from within an event loop callback (i.e., when
/// EventLoop::Current() returns a valid loop).
///
/// Example:
///   co_await Sleep(std::chrono::milliseconds(100));
Future<void> Sleep(std::chrono::nanoseconds duration);

/// Returns a Future<void> that resolves on the next event loop tick.
/// Equivalent to `Sleep(0)` — yields control to allow other callbacks to run.
///
/// Example:
///   co_await Yield();
Future<void> Yield();

}  // namespace asyncio

#endif  // ASYNCIO_SLEEP_H_
