// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for Task<T> and AsyncTaskPromise<T>.

#include <chrono>
#include <string>

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/sleep.h"
#include "asyncio/task.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

// --- Helper: run a coroutine-based test inside an event loop ---

/// Runs a simple coroutine that returns an int.
Task<int> SimpleReturn() { co_return 42; }

TEST(TaskTest, BasicCoReturn) {
  EventLoop loop;
  int result = 0;
  bool done = false;

  loop.CallSoon([&]() {
    auto task = SimpleReturn();
    task.AddDoneCallback([&](Future<int>& f) {
      result = f.Result();
      done = true;
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(done);
  EXPECT_EQ(result, 42);
}

/// Runs a coroutine that returns void.
Task<void> SimpleVoid() { co_return; }

TEST(TaskTest, VoidCoReturn) {
  EventLoop loop;
  bool done = false;

  loop.CallSoon([&]() {
    auto task = SimpleVoid();
    task.AddDoneCallback([&](Future<void>&) {
      done = true;
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(done);
}

/// Coroutine that co_awaits a Future<int>.
Task<int> AwaitFutureValue() {
  Future<int> fut;
  // Schedule resolution on next tick.
  EventLoop::Current()->CallSoon([f = fut]() mutable { f.SetResult(99); });
  int val = co_await fut;
  co_return val;
}

TEST(TaskTest, AwaitFutureInt) {
  EventLoop loop;
  int result = 0;

  loop.CallSoon([&]() {
    auto task = AwaitFutureValue();
    task.AddDoneCallback([&](Future<int>& f) {
      result = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(result, 99);
}

/// Coroutine that co_awaits a Future<void>.
Task<int> AwaitFutureVoid() {
  Future<void> fut;
  EventLoop::Current()->CallSoon([f = fut]() mutable { f.SetResult(); });
  co_await fut;
  co_return 7;
}

TEST(TaskTest, AwaitFutureVoid) {
  EventLoop loop;
  int result = 0;

  loop.CallSoon([&]() {
    auto task = AwaitFutureVoid();
    task.AddDoneCallback([&](Future<int>& f) {
      result = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(result, 7);
}

/// Coroutine that co_awaits Sleep.
Task<int> AwaitSleep() {
  co_await Sleep(std::chrono::milliseconds(5));
  co_return 123;
}

TEST(TaskTest, AwaitSleep) {
  EventLoop loop;
  int result = 0;

  loop.CallSoon([&]() {
    auto task = AwaitSleep();
    task.AddDoneCallback([&](Future<int>& f) {
      result = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(result, 123);
}

/// Coroutine that throws an exception.
Task<int> ThrowException() {
  throw std::runtime_error("oops");
  co_return 0;
}

TEST(TaskTest, ExceptionPropagation) {
  EventLoop loop;
  bool caught = false;

  loop.CallSoon([&]() {
    auto task = ThrowException();
    task.AddDoneCallback([&](Future<int>& f) {
      try {
        f.Result();
      } catch (const std::runtime_error& e) {
        caught = std::string(e.what()) == "oops";
      }
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(caught);
}

/// Coroutine that chains multiple co_awaits.
Task<int> ChainAwaits() {
  co_await Sleep(std::chrono::milliseconds(1));
  co_await Yield();
  Future<int> fut;
  EventLoop::Current()->CallSoon([f = fut]() mutable { f.SetResult(10); });
  int v = co_await fut;
  co_return v + 5;
}

TEST(TaskTest, ChainedAwaits) {
  EventLoop loop;
  int result = 0;

  loop.CallSoon([&]() {
    auto task = ChainAwaits();
    task.AddDoneCallback([&](Future<int>& f) {
      result = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(result, 15);
}

// --- Cancellation tests ---

/// Coroutine that sleeps then returns — used for cancellation tests.
Task<int> SleeperTask() {
  co_await Sleep(std::chrono::milliseconds(500));
  co_return 999;
}

TEST(TaskTest, CancelTaskBeforeSleepResolves) {
  EventLoop loop;
  bool done = false;
  bool cancelled = false;

  loop.CallSoon([&]() {
    auto task = SleeperTask();
    task.AddDoneCallback([&](Future<int>& f) {
      done = true;
      cancelled = f.Cancelled();
      loop.Stop();
    });
    // Cancel on the next tick, before the sleep resolves.
    loop.CallSoon([task]() mutable { task.Cancel(); });
    // Safety timeout.
    loop.CallLater(std::chrono::milliseconds(100),
                   [&]() { loop.Stop(); });
  });

  loop.RunForever();
  EXPECT_TRUE(done);
  EXPECT_TRUE(cancelled);
}

TEST(TaskTest, CancelReturnsFalseWhenDone) {
  EventLoop loop;
  bool cancel_result = true;

  loop.CallSoon([&]() {
    auto task = SimpleReturn();
    // Wait for task to complete.
    task.AddDoneCallback([&](Future<int>&) {
      cancel_result = task.Cancel();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_FALSE(cancel_result);
}

TEST(TaskTest, CancellingAndUncancel) {
  EventLoop loop;
  int cancelling_count = -1;

  loop.CallSoon([&]() {
    auto task = SleeperTask();
    EXPECT_EQ(task.Cancelling(), 0);

    task.Cancel();
    EXPECT_EQ(task.Cancelling(), 1);

    task.Uncancel();
    EXPECT_EQ(task.Cancelling(), 0);

    cancelling_count = task.Cancelling();

    // Safety: stop the loop.
    task.AddDoneCallback([&](Future<int>&) { loop.Stop(); });
    loop.CallLater(std::chrono::milliseconds(100),
                   [&]() { loop.Stop(); });
  });

  loop.RunForever();
  EXPECT_EQ(cancelling_count, 0);
}

// --- Task name ---

TEST(TaskTest, SetAndGetName) {
  EventLoop loop;
  std::string name;

  loop.CallSoon([&]() {
    auto task = SimpleReturn();
    task.SetName("my-task");
    name = task.GetName();
    task.AddDoneCallback([&](Future<int>&) { loop.Stop(); });
  });

  loop.RunForever();
  EXPECT_EQ(name, "my-task");
}

// --- Task is a Future ---

TEST(TaskTest, TaskIsFuture) {
  EventLoop loop;
  bool done = false;

  loop.CallSoon([&]() {
    auto task = SimpleReturn();
    Future<int>& fut = task;
    fut.AddDoneCallback([&](Future<int>& f) {
      done = f.Done();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(done);
}

// --- Multiple tasks concurrently ---

Task<int> AddAsync(int a, int b) {
  co_await Sleep(std::chrono::milliseconds(1));
  co_return a + b;
}

TEST(TaskTest, MultipleConcurrentTasks) {
  EventLoop loop;
  int count = 0;
  int r1 = 0, r2 = 0;

  loop.CallSoon([&]() {
    auto t1 = AddAsync(1, 2);
    auto t2 = AddAsync(10, 20);

    t1.AddDoneCallback([&](Future<int>& f) {
      r1 = f.Result();
      count++;
      if (count == 2) loop.Stop();
    });
    t2.AddDoneCallback([&](Future<int>& f) {
      r2 = f.Result();
      count++;
      if (count == 2) loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(count, 2);
  EXPECT_EQ(r1, 3);
  EXPECT_EQ(r2, 30);
}

}  // namespace
}  // namespace asyncio
