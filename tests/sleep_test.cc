// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for Sleep() and Yield() functions.

#include <chrono>

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/sleep.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

// --- Sleep ---

TEST(SleepTest, SleepResolvesAfterDuration) {
  EventLoop loop;
  bool resolved = false;

  loop.CallSoon([&]() {
    auto future = Sleep(std::chrono::milliseconds(10));
    future.AddDoneCallback([&](Future<void>&) {
      resolved = true;
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(resolved);
}

TEST(SleepTest, SleepFutureIsPendingInitially) {
  EventLoop loop;
  bool checked = false;

  loop.CallSoon([&]() {
    auto future = Sleep(std::chrono::milliseconds(50));
    EXPECT_FALSE(future.Done());
    checked = true;
    future.AddDoneCallback([&](Future<void>&) { loop.Stop(); });
  });

  loop.RunForever();
  EXPECT_TRUE(checked);
}

TEST(SleepTest, MultipleSleepsResolve) {
  EventLoop loop;
  int count = 0;

  loop.CallSoon([&]() {
    // Schedule two sleeps with different durations.
    auto f1 = Sleep(std::chrono::milliseconds(5));
    auto f2 = Sleep(std::chrono::milliseconds(10));

    f1.AddDoneCallback([&](Future<void>&) {
      count++;
      if (count == 2) loop.Stop();
    });
    f2.AddDoneCallback([&](Future<void>&) {
      count++;
      if (count == 2) loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(count, 2);
}

TEST(SleepTest, CancelledSleepDoesNotResolve) {
  EventLoop loop;
  bool resolved = false;

  loop.CallSoon([&]() {
    auto future = Sleep(std::chrono::milliseconds(50));
    future.AddDoneCallback([&](Future<void>& f) {
      // Only count non-cancelled resolution (i.e., SetResult was called).
      if (!f.Cancelled()) {
        resolved = true;
      }
      loop.Stop();
    });
    // Cancel immediately — the timer's SetResult callback won't fire.
    future.Cancel();
    // Safety stop in case the done callback doesn't stop the loop.
    loop.CallLater(std::chrono::milliseconds(100),
                   [&]() { loop.Stop(); });
  });

  loop.RunForever();
  EXPECT_FALSE(resolved);
}

// --- Yield ---

TEST(YieldTest, YieldResolvesOnNextTick) {
  EventLoop loop;
  int order = 0;
  int yield_order = 0;

  loop.CallSoon([&]() {
    order++;
    EXPECT_EQ(order, 1);

    auto future = Yield();
    future.AddDoneCallback([&](Future<void>&) {
      yield_order = ++order;
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(yield_order, 2);
}

TEST(YieldTest, YieldAllowsOtherCallbacksToRun) {
  EventLoop loop;
  std::vector<int> order;

  loop.CallSoon([&]() {
    order.push_back(1);

    // This callback should run before the Yield resolves.
    loop.CallSoon([&]() { order.push_back(3); });

    auto future = Yield();
    future.AddDoneCallback([&](Future<void>&) {
      order.push_back(2);  // Actually, Yield resolves on next tick.
      // The "3" callback might have run first depending on order.
      loop.Stop();
    });
  });

  loop.RunForever();
  // Yield uses CallSoon, so the Yield callback is queued after the
  // pre-existing callback "3". Order should be: 1, 3, 2.
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 3);
  EXPECT_EQ(order[2], 2);
}

// --- Integration: Sleep with co_await ---

// Minimal coroutine type for testing co_await with Sleep.
struct TestCoro {
  struct promise_type {
    TestCoro get_return_object() { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { std::terminate(); }
  };
};

TestCoro AwaitSleepAndStop(EventLoop& loop, bool& done) {
  co_await Sleep(std::chrono::milliseconds(5));
  done = true;
  loop.Stop();
}

TEST(SleepTest, AwaitSleepCoroutine) {
  EventLoop loop;
  bool done = false;

  loop.CallSoon([&]() { AwaitSleepAndStop(loop, done); });

  loop.RunForever();
  EXPECT_TRUE(done);
}

}  // namespace
}  // namespace asyncio
