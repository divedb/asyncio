// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Shield — detach-on-cancel wrapper for Futures.

#ifndef ASYNCIO_SHIELD_H_
#define ASYNCIO_SHIELD_H_

#include <exception>
#include <type_traits>
#include <utility>

#include "asyncio/future.h"

namespace asyncio {

/// Creates a wrapper Future that mirrors the inner Future's result or
/// exception, but does NOT propagate cancellation from the wrapper to the
/// inner Future.
///
/// If the wrapper is cancelled, the inner Future continues running.
/// If the inner Future is cancelled, the wrapper is also cancelled.
///
/// This mirrors Python's `asyncio.shield()`.
template <typename T>
Future<T> Shield(Future<T> inner) {
  Future<T> wrapper;
  inner.AddDoneCallback([wrapper](Future<T>& f) mutable {
    // If the wrapper was already cancelled externally, ignore inner's result.
    if (wrapper.Done()) return;
    if (f.Cancelled()) {
      wrapper.Cancel();
    } else if (f.GetException()) {
      wrapper.SetException(f.GetException());
    } else {
      if constexpr (std::is_void_v<T>) {
        wrapper.SetResult();
      } else {
        wrapper.SetResult(std::move(f.Result()));
      }
    }
  });
  return wrapper;
}

}  // namespace asyncio

#endif  // ASYNCIO_SHIELD_H_
