// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Gather — concurrent task composition.

#ifndef ASYNCIO_GATHER_H_
#define ASYNCIO_GATHER_H_

#include <exception>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "asyncio/future.h"
#include "asyncio/task.h"

namespace asyncio {

// ---------------------------------------------------------------------------
// Gather — vector overload for homogeneous Futures.
// ---------------------------------------------------------------------------

/// Runs multiple Futures concurrently and collects their results.
///
/// All input Futures must already be running (eager tasks). The Gather
/// coroutine awaits each Future in order; since the backing tasks run
/// concurrently on the event loop, the total wall time is determined by
/// the slowest task, not the sum.
///
/// If any Future throws, the exception propagates and the Gather task fails.
/// Remaining tasks continue running but their results are ignored.
///
/// This mirrors Python's `asyncio.gather()` without return_exceptions.
template <typename T>
  requires(!std::is_void_v<T>)
Task<std::vector<T>> Gather(std::vector<Future<T>> futures) {
  std::vector<T> results;
  results.reserve(futures.size());
  for (auto& fut : futures) {
    results.push_back(co_await fut);
  }
  co_return results;
}

/// Overload for void Futures. Awaits all Futures concurrently; the Gather
/// task completes when all are done.
Task<void> Gather(std::vector<Future<void>> futures) {
  for (auto& fut : futures) {
    co_await fut;
  }
}

// ---------------------------------------------------------------------------
// Gather — variadic overload for heterogeneous Futures.
// ---------------------------------------------------------------------------

namespace detail {

template <typename... Ts, size_t... Is>
Task<std::tuple<Ts...>> GatherTupleImpl(
    std::tuple<Future<Ts>...> futures, std::index_sequence<Is...>) {
  std::tuple<Ts...> results;
  // Fold expression: co_await each Future and store in the corresponding
  // tuple element. The comma operator sequences the evaluations.
  ((std::get<Is>(results) = co_await std::get<Is>(futures)), ...);
  co_return results;
}

}  // namespace detail

/// Variadic Gather: runs heterogeneous Futures concurrently and returns a
/// tuple of results.
template <typename... Ts>
Task<std::tuple<Ts...>> Gather(Future<Ts>... futures) {
  return detail::GatherTupleImpl<Ts...>(
      std::make_tuple(std::move(futures)...),
      std::index_sequence_for<Ts...>{});
}

// ---------------------------------------------------------------------------
// GatherWithExceptions — collects results and exceptions.
// ---------------------------------------------------------------------------

/// Like Gather, but catches exceptions from each Future and returns them
/// as part of the result vector. Each element is either a value or an
/// exception_ptr. The Gather task always succeeds (unless cancelled).
template <typename T>
  requires(!std::is_void_v<T>)
Task<std::vector<std::variant<T, std::exception_ptr>>>
GatherWithExceptions(std::vector<Future<T>> futures) {
  std::vector<std::variant<T, std::exception_ptr>> results;
  results.reserve(futures.size());
  for (auto& fut : futures) {
    try {
      results.push_back(co_await fut);
    } catch (...) {
      results.push_back(std::current_exception());
    }
  }
  co_return results;
}

}  // namespace asyncio

#endif  // ASYNCIO_GATHER_H_
