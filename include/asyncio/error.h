// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Exception types for the asyncio library.

#ifndef ASYNCIO_ERROR_H_
#define ASYNCIO_ERROR_H_

#include <stdexcept>
#include <string>

namespace asyncio {

/// Base exception for all asyncio errors.
class AsyncError : public std::runtime_error {
  using std::runtime_error::runtime_error;
};

/// Thrown when a Future or Task is cancelled.
/// Mirrors Python's `asyncio.CancelledError`.
class AsyncCancelledError : public AsyncError {
 public:
  AsyncCancelledError() : AsyncError("Future was cancelled") {}
};

/// Thrown when an operation is performed on a Future in an invalid state
/// (e.g., setting a result on an already-resolved Future).
class InvalidStateError : public AsyncError {
  using AsyncError::AsyncError;
};

/// Thrown when an async operation exceeds its deadline.
/// Mirrors Python's `asyncio.TimeoutError`.
class AsyncTimeoutError : public AsyncError {
 public:
  AsyncTimeoutError() : AsyncError("Operation timed out") {}
};

}  // namespace asyncio

#endif  // ASYNCIO_ERROR_H_
