// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for Future<T> and Future<void>: state transitions, done callbacks,
// exception propagation, and awaitable mechanics.

#include <chrono>
#include <exception>
#include <string>

#include "asyncio/error.h"
#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

// --- Helper: minimal coroutine return type for testing co_await ---

struct TestCoro {
  struct promise_type {
    TestCoro get_return_object() { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { std::terminate(); }
  };
};

// Helper coroutine: awaits a Future<int> and stores the result.
TestCoro AwaitAndStore(Future<int>& fut, int& result) {
  result = co_await fut;
}

// Helper coroutine: awaits a Future<void> and sets a flag.
TestCoro AwaitVoidAndSet(Future<void>& fut, bool& done) {
  co_await fut;
  done = true;
}

// Helper coroutine: awaits a Future<int> and catches cancellation.
TestCoro AwaitCatchCancel(Future<int>& fut, bool& caught) {
  try {
    co_await fut;
  } catch (const AsyncCancelledError&) {
    caught = true;
  }
}

// Helper coroutine: awaits a Future<void> and catches exception.
TestCoro AwaitCatchException(Future<void>& fut, bool& caught,
                             std::string& message) {
  try {
    co_await fut;
  } catch (const std::runtime_error& e) {
    caught = true;
    message = e.what();
  }
}

// ============================================================
// Future<int> — state transitions
// ============================================================

TEST(FutureTest, InitiallyPending) {
  Future<int> fut;
  EXPECT_FALSE(fut.Done());
  EXPECT_FALSE(fut.Cancelled());
}

TEST(FutureTest, SetResultTransitionsToFinished) {
  Future<int> fut;
  fut.SetResult(42);
  EXPECT_TRUE(fut.Done());
  EXPECT_FALSE(fut.Cancelled());
  EXPECT_EQ(fut.Result(), 42);
}

TEST(FutureTest, SetResultMovesValue) {
  Future<std::string> fut;
  std::string value = "hello";
  fut.SetResult(std::move(value));
  EXPECT_EQ(fut.Result(), "hello");
}

TEST(FutureTest, SetResultTwiceThrows) {
  Future<int> fut;
  fut.SetResult(1);
  EXPECT_THROW(fut.SetResult(2), InvalidStateError);
}

TEST(FutureTest, SetResultAfterCancelThrows) {
  Future<int> fut;
  fut.Cancel();
  EXPECT_THROW(fut.SetResult(1), InvalidStateError);
}

TEST(FutureTest, ResultOnPendingThrows) {
  Future<int> fut;
  EXPECT_THROW(fut.Result(), InvalidStateError);
}

TEST(FutureTest, CancelTransitionsToCancelled) {
  Future<int> fut;
  EXPECT_TRUE(fut.Cancel());
  EXPECT_TRUE(fut.Done());
  EXPECT_TRUE(fut.Cancelled());
}

TEST(FutureTest, CancelTwiceReturnsFalse) {
  Future<int> fut;
  EXPECT_TRUE(fut.Cancel());
  EXPECT_FALSE(fut.Cancel());
}

TEST(FutureTest, CancelAfterSetResultReturnsFalse) {
  Future<int> fut;
  fut.SetResult(42);
  EXPECT_FALSE(fut.Cancel());
}

TEST(FutureTest, ResultOnCancelledThrows) {
  Future<int> fut;
  fut.Cancel();
  EXPECT_THROW(fut.Result(), AsyncCancelledError);
}

// ============================================================
// Exception handling
// ============================================================

TEST(FutureTest, SetExceptionTransitionsToFinished) {
  Future<int> fut;
  fut.SetException(std::make_exception_ptr(std::runtime_error("boom")));
  EXPECT_TRUE(fut.Done());
  EXPECT_FALSE(fut.Cancelled());
  EXPECT_THROW(fut.Result(), std::runtime_error);
}

TEST(FutureTest, SetExceptionTwiceThrows) {
  Future<int> fut;
  auto ex = std::make_exception_ptr(std::runtime_error("a"));
  fut.SetException(ex);
  EXPECT_THROW(fut.SetException(ex), InvalidStateError);
}

TEST(FutureTest, GetExceptionReturnsStoredException) {
  Future<int> fut;
  auto ex = std::make_exception_ptr(std::runtime_error("err"));
  fut.SetException(ex);
  EXPECT_TRUE(fut.GetException());
  try {
    std::rethrow_exception(fut.GetException());
  } catch (const std::runtime_error& e) {
    EXPECT_STREQ(e.what(), "err");
  }
}

TEST(FutureTest, GetExceptionNullWhenPending) {
  Future<int> fut;
  EXPECT_FALSE(fut.GetException());
}

TEST(FutureTest, GetExceptionNullWhenResultSet) {
  Future<int> fut;
  fut.SetResult(10);
  EXPECT_FALSE(fut.GetException());
}

// ============================================================
// Shared state (copy semantics)
// ============================================================

TEST(FutureTest, CopiesShareState) {
  Future<int> fut;
  auto copy = fut;
  copy.SetResult(7);
  EXPECT_TRUE(fut.Done());
  EXPECT_EQ(fut.Result(), 7);
}

TEST(FutureTest, CancelOnCopyCancelsOriginal) {
  Future<int> fut;
  auto copy = fut;
  copy.Cancel();
  EXPECT_TRUE(fut.Cancelled());
}

// ============================================================
// Done callbacks
// ============================================================

TEST(FutureTest, DoneCallbackCalledOnSetResult) {
  Future<int> fut;
  bool called = false;
  fut.AddDoneCallback([&](Future<int>& f) {
    called = true;
    EXPECT_EQ(f.Result(), 42);
  });
  EXPECT_FALSE(called);
  fut.SetResult(42);
  EXPECT_TRUE(called);
}

TEST(FutureTest, DoneCallbackCalledOnCancel) {
  Future<int> fut;
  bool called = false;
  fut.AddDoneCallback([&](Future<int>& f) {
    called = true;
    EXPECT_TRUE(f.Cancelled());
  });
  fut.Cancel();
  EXPECT_TRUE(called);
}

TEST(FutureTest, DoneCallbackCalledOnException) {
  Future<int> fut;
  bool called = false;
  fut.AddDoneCallback([&](Future<int>& f) {
    called = true;
    EXPECT_TRUE(f.GetException());
  });
  fut.SetException(std::make_exception_ptr(std::runtime_error("err")));
  EXPECT_TRUE(called);
}

TEST(FutureTest, DoneCallbackCalledImmediatelyIfAlreadyDone) {
  Future<int> fut;
  fut.SetResult(10);
  bool called = false;
  fut.AddDoneCallback([&](Future<int>&) { called = true; });
  EXPECT_TRUE(called);
}

TEST(FutureTest, MultipleDoneCallbacksAllCalled) {
  Future<int> fut;
  int count = 0;
  fut.AddDoneCallback([&](Future<int>&) { count++; });
  fut.AddDoneCallback([&](Future<int>&) { count++; });
  fut.AddDoneCallback([&](Future<int>&) { count++; });
  fut.SetResult(1);
  EXPECT_EQ(count, 3);
}

// ============================================================
// Awaitable — await_ready / await_resume (no suspension)
// ============================================================

TEST(FutureTest, AwaiterReadyWhenDone) {
  Future<int> fut;
  fut.SetResult(42);
  auto awaiter = fut.operator co_await();
  EXPECT_TRUE(awaiter.await_ready());
  EXPECT_EQ(awaiter.await_resume(), 42);
}

TEST(FutureTest, AwaiterNotReadyWhenPending) {
  Future<int> fut;
  auto awaiter = fut.operator co_await();
  EXPECT_FALSE(awaiter.await_ready());
}

// ============================================================
// Awaitable — suspension and resumption
// ============================================================

TEST(FutureTest, AwaitSuspendsUntilResolved) {
  Future<int> fut;
  int result = 0;

  AwaitAndStore(fut, result);

  // Coroutine should be suspended.
  EXPECT_EQ(result, 0);

  // Resolve the Future — this resumes the coroutine inline.
  fut.SetResult(42);
  EXPECT_EQ(result, 42);
}

TEST(FutureTest, AwaitVoidSuspendsUntilResolved) {
  Future<void> fut;
  bool done = false;

  AwaitVoidAndSet(fut, done);
  EXPECT_FALSE(done);

  fut.SetResult();
  EXPECT_TRUE(done);
}

TEST(FutureTest, AwaitCancelledFutureThrows) {
  Future<int> fut;
  bool caught = false;

  AwaitCatchCancel(fut, caught);

  fut.Cancel();

  EXPECT_TRUE(caught);
}

TEST(FutureTest, AwaitExceptionFutureRethrows) {
  Future<void> fut;
  bool caught = false;
  std::string message;

  AwaitCatchException(fut, caught, message);

  fut.SetException(std::make_exception_ptr(std::runtime_error("test_err")));

  EXPECT_TRUE(caught);
  EXPECT_EQ(message, "test_err");
}

// ============================================================
// Future<void> specialization
// ============================================================

TEST(FutureVoidTest, InitiallyPending) {
  Future<void> fut;
  EXPECT_FALSE(fut.Done());
  EXPECT_FALSE(fut.Cancelled());
}

TEST(FutureVoidTest, SetResultTransitionsToFinished) {
  Future<void> fut;
  fut.SetResult();
  EXPECT_TRUE(fut.Done());
  EXPECT_FALSE(fut.Cancelled());
}

TEST(FutureVoidTest, ResultValidatesSuccessfullyWhenFinished) {
  Future<void> fut;
  fut.SetResult();
  EXPECT_NO_THROW(fut.Result());
}

TEST(FutureVoidTest, ResultThrowsWhenPending) {
  Future<void> fut;
  EXPECT_THROW(fut.Result(), InvalidStateError);
}

TEST(FutureVoidTest, ResultThrowsWhenCancelled) {
  Future<void> fut;
  fut.Cancel();
  EXPECT_THROW(fut.Result(), AsyncCancelledError);
}

TEST(FutureVoidTest, ResultRethrowsException) {
  Future<void> fut;
  fut.SetException(std::make_exception_ptr(std::runtime_error("err")));
  EXPECT_THROW(fut.Result(), std::runtime_error);
}

TEST(FutureVoidTest, DoneCallbackOnSetResult) {
  Future<void> fut;
  bool called = false;
  fut.AddDoneCallback([&](Future<void>&) { called = true; });
  fut.SetResult();
  EXPECT_TRUE(called);
}

TEST(FutureVoidTest, CopiesShareState) {
  Future<void> fut;
  auto copy = fut;
  copy.SetResult();
  EXPECT_TRUE(fut.Done());
}

// ============================================================
// EventLoop::Current() integration
// ============================================================

TEST(EventLoopTest, CurrentReturnsNullOutsideRunForever) {
  EXPECT_EQ(EventLoop::Current(), nullptr);
}

TEST(EventLoopTest, CurrentReturnsLoopInsideRunForever) {
  EventLoop loop;
  EventLoop* ptr = nullptr;

  loop.CallSoon([&]() {
    ptr = EventLoop::Current();
    loop.Stop();
  });

  loop.RunForever();
  EXPECT_EQ(ptr, &loop);
  EXPECT_EQ(EventLoop::Current(), nullptr);
}

}  // namespace
}  // namespace asyncio
