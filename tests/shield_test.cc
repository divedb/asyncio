// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for Shield().

#include <chrono>

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/shield.h"
#include "asyncio/sleep.h"
#include "asyncio/task.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

// --- Shield basic ---

TEST(ShieldTest, ShieldPassesThroughResult) {
  EventLoop loop;
  int result = 0;

  loop.CallSoon([&]() {
    Future<int> inner;
    auto shielded = Shield(inner);

    shielded.AddDoneCallback([&](Future<int>& f) {
      result = f.Result();
      loop.Stop();
    });

    // Resolve inner after shield is set up.
    inner.SetResult(42);
  });

  loop.RunForever();
  EXPECT_EQ(result, 42);
}

TEST(ShieldTest, ShieldPassesThroughException) {
  EventLoop loop;
  bool caught = false;

  loop.CallSoon([&]() {
    Future<int> inner;
    auto shielded = Shield(inner);

    shielded.AddDoneCallback([&](Future<int>& f) {
      try {
        f.Result();
      } catch (const std::runtime_error& e) {
        caught = std::string(e.what()) == "shield_err";
      }
      loop.Stop();
    });

    inner.SetException(
        std::make_exception_ptr(std::runtime_error("shield_err")));
  });

  loop.RunForever();
  EXPECT_TRUE(caught);
}

TEST(ShieldTest, CancellingWrapperDoesNotCancelInner) {
  EventLoop loop;
  bool wrapper_cancelled = false;
  bool inner_resolved = false;

  loop.CallSoon([&]() {
    Future<void> inner;
    auto shielded = Shield(inner);

    shielded.AddDoneCallback([&](Future<void>& f) {
      wrapper_cancelled = f.Cancelled();
      loop.Stop();
    });

    // Cancel the wrapper — inner should NOT be cancelled.
    shielded.Cancel();
    EXPECT_FALSE(inner.Done());

    // Resolve inner normally.
    inner.SetResult();
    inner_resolved = true;
  });

  loop.RunForever();
  EXPECT_TRUE(wrapper_cancelled);
  EXPECT_TRUE(inner_resolved);
}

TEST(ShieldTest, CancellingInnerCancelsWrapper) {
  EventLoop loop;
  bool wrapper_cancelled = false;

  loop.CallSoon([&]() {
    Future<void> inner;
    auto shielded = Shield(inner);

    shielded.AddDoneCallback([&](Future<void>& f) {
      wrapper_cancelled = f.Cancelled();
      loop.Stop();
    });

    // Cancel the inner — wrapper should also be cancelled.
    inner.Cancel();
  });

  loop.RunForever();
  EXPECT_TRUE(wrapper_cancelled);
}

// --- Shield with Task ---

Task<int> ShieldedTask() {
  co_await Sleep(std::chrono::milliseconds(5));
  co_return 99;
}

TEST(ShieldTest, ShieldWithRunningTask) {
  EventLoop loop;
  int result = 0;

  loop.CallSoon([&]() {
    auto task = ShieldedTask();
    auto shielded = Shield<int>(task);

    shielded.AddDoneCallback([&](Future<int>& f) {
      result = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(result, 99);
}

TEST(ShieldTest, ShieldVoidFuture) {
  EventLoop loop;
  bool done = false;

  loop.CallSoon([&]() {
    Future<void> inner;
    auto shielded = Shield(inner);

    shielded.AddDoneCallback([&](Future<void>&) {
      done = true;
      loop.Stop();
    });

    inner.SetResult();
  });

  loop.RunForever();
  EXPECT_TRUE(done);
}

}  // namespace
}  // namespace asyncio
