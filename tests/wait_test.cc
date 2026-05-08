// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for WaitFor() and TimeoutScope.

#include <chrono>

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/sleep.h"
#include "asyncio/task.h"
#include "asyncio/wait.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

// --- WaitFor ---

TEST(WaitForTest, CompletesBeforeTimeout) {
  EventLoop loop;
  int result = 0;

  loop.CallSoon([&]() {
    Future<int> inner;
    auto wrapped = WaitFor(inner, std::chrono::milliseconds(100));

    wrapped.AddDoneCallback([&](Future<int>& f) {
      result = f.Result();
      loop.Stop();
    });

    // Resolve quickly.
    loop.CallSoon([inner]() mutable { inner.SetResult(42); });
  });

  loop.RunForever();
  EXPECT_EQ(result, 42);
}

TEST(WaitForTest, TimesOut) {
  EventLoop loop;
  bool timed_out = false;

  loop.CallSoon([&]() {
    Future<int> inner;
    auto wrapped = WaitFor(inner, std::chrono::milliseconds(5));

    wrapped.AddDoneCallback([&](Future<int>& f) {
      try {
        f.Result();
      } catch (const AsyncTimeoutError&) {
        timed_out = true;
      }
      loop.Stop();
    });
    // Never resolve inner — should timeout.
  });

  loop.RunForever();
  EXPECT_TRUE(timed_out);
}

TEST(WaitForTest, TimeoutCancelsInner) {
  EventLoop loop;
  bool inner_cancelled = false;

  loop.CallSoon([&]() {
    Future<void> inner;
    inner.AddDoneCallback([&](Future<void>& f) {
      inner_cancelled = f.Cancelled();
      loop.Stop();
    });
    auto wrapped = WaitFor(inner, std::chrono::milliseconds(5));
    // Ignore wrapped's result.
  });

  loop.RunForever();
  EXPECT_TRUE(inner_cancelled);
}

TEST(WaitForTest, VoidFutureCompletesBeforeTimeout) {
  EventLoop loop;
  bool done = false;

  loop.CallSoon([&]() {
    Future<void> inner;
    auto wrapped = WaitFor(inner, std::chrono::milliseconds(100));

    wrapped.AddDoneCallback([&](Future<void>&) {
      done = true;
      loop.Stop();
    });

    loop.CallSoon([inner]() mutable { inner.SetResult(); });
  });

  loop.RunForever();
  EXPECT_TRUE(done);
}

// --- WaitFor with Task ---

Task<int> QuickTask() {
  co_await Sleep(std::chrono::milliseconds(1));
  co_return 77;
}

Task<int> SlowTask() {
  co_await Sleep(std::chrono::milliseconds(500));
  co_return 999;
}

TEST(WaitForTest, WaitForQuickTask) {
  EventLoop loop;
  int result = 0;

  loop.CallSoon([&]() {
    auto task = QuickTask();
    auto wrapped = WaitFor<int>(task, std::chrono::milliseconds(100));

    wrapped.AddDoneCallback([&](Future<int>& f) {
      result = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(result, 77);
}

TEST(WaitForTest, WaitForSlowTaskTimesOut) {
  EventLoop loop;
  bool timed_out = false;

  loop.CallSoon([&]() {
    auto task = SlowTask();
    auto wrapped = WaitFor<int>(task, std::chrono::milliseconds(5));

    wrapped.AddDoneCallback([&](Future<int>& f) {
      try {
        f.Result();
      } catch (const AsyncTimeoutError&) {
        timed_out = true;
      }
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(timed_out);
}

// --- TimeoutScope (tested indirectly via WaitFor) ---

Task<int> TimeoutScopeHelper(std::chrono::nanoseconds sleep_dur,
                              std::chrono::nanoseconds timeout_dur) {
  // Create an inner task to apply the timeout scope to.
  // For testing, we use the current task approach.
  auto inner_task = [&]() -> Task<int> {
    co_await Sleep(sleep_dur);
    co_return 42;
  }();

  auto wrapped = WaitFor<int>(inner_task, timeout_dur);
  co_return co_await wrapped;
}

TEST(TimeoutScopeTest, FastOperationSucceeds) {
  EventLoop loop;
  int result = 0;

  loop.CallSoon([&]() {
    auto task = TimeoutScopeHelper(
        std::chrono::milliseconds(1),
        std::chrono::milliseconds(100));

    task.AddDoneCallback([&](Future<int>& f) {
      result = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(result, 42);
}

TEST(TimeoutScopeTest, SlowOperationTimesOut) {
  EventLoop loop;
  bool timed_out = false;

  loop.CallSoon([&]() {
    auto task = TimeoutScopeHelper(
        std::chrono::milliseconds(500),
        std::chrono::milliseconds(5));

    task.AddDoneCallback([&](Future<int>& f) {
      try {
        f.Result();
      } catch (const AsyncTimeoutError&) {
        timed_out = true;
      }
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(timed_out);
}

}  // namespace
}  // namespace asyncio
