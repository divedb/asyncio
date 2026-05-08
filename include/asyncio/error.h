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

/// Thrown when an AsyncBarrier is broken (abort() was called).
/// Mirrors Python's `threading.BrokenBarrierError`.
class BrokenBarrierError : public AsyncError {
 public:
  BrokenBarrierError() : AsyncError("Barrier is broken") {}
};

/// Thrown when operating on a shut-down AsyncQueue.
/// Mirrors Python's `asyncio.QueueShutDown`.
class QueueShutDownError : public AsyncError {
 public:
  QueueShutDownError() : AsyncError("Queue is shut down") {}
};

/// Thrown when a non-blocking Put() is called on a full bounded queue.
/// Mirrors Python's `asyncio.QueueFull`.
class QueueFullError : public AsyncError {
 public:
  QueueFullError() : AsyncError("Queue is full") {}
};

/// Thrown when a non-blocking Get() is called on an empty queue.
/// Mirrors Python's `asyncio.QueueEmpty`.
class QueueEmptyError : public AsyncError {
 public:
  QueueEmptyError() : AsyncError("Queue is empty") {}
};

/// Thrown when StreamReader::ReadExactly() cannot read the exact number
/// of bytes before EOF.
class IncompleteReadError : public AsyncError {
 public:
  IncompleteReadError(int expected, int actual);
};

/// Thrown when StreamReader::ReadUntil() exceeds the configured buffer
/// limit without finding the separator.
class LimitOverrunError : public AsyncError {
 public:
  explicit LimitOverrunError(const std::string& msg);
};

// Inline implementations of constructors for header-only convenience.
inline IncompleteReadError::IncompleteReadError(int expected, int actual)
    : AsyncError("IncompleteReadError: expected " + std::to_string(expected) +
                 ", received " + std::to_string(actual)) {}

inline LimitOverrunError::LimitOverrunError(const std::string& msg)
    : AsyncError("LimitOverrunError: " + msg) {}

}  // namespace asyncio

#endif  // ASYNCIO_ERROR_H_
