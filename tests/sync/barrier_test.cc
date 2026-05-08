// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for AsyncBarrier.

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/sleep.h"
#include "asyncio/sync/barrier.h"
#include "asyncio/task.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

TEST(AsyncBarrierTest, PartiesAndNWaiting) {
  AsyncBarrier barrier(3);
  EXPECT_EQ(barrier.Parties(), 3);
  EXPECT_EQ(barrier.NWaiting(), 0);
}

TEST(AsyncBarrierTest, SinglePartyPassesImmediately) {
  EventLoop loop;
  bool passed = false;

  loop.CallSoon([&]() {
    AsyncBarrier barrier(1);
    auto f = barrier.Wait();
    EXPECT_TRUE(f.Done());
    passed = true;
    loop.Stop();
  });

  loop.RunForever();
  EXPECT_TRUE(passed);
}

TEST(AsyncBarrierTest, AllPartiesPass) {
  EventLoop loop;
  int passed_count = 0;

  loop.CallSoon([&]() {
    auto barrier = std::make_shared<AsyncBarrier>(3);

    auto waiter = [&]() -> Task<void> {
      co_await barrier->Wait();
      passed_count++;
    };

    auto t1 = waiter();
    auto t2 = waiter();
    auto t3 = waiter();

    auto check = [&]() -> Task<void> {
      co_await t1;
      co_await t2;
      co_await t3;
    };

    auto tc = check();
    tc.AddDoneCallback([&](Future<void>&) { loop.Stop(); });
  });

  loop.RunForever();
  EXPECT_EQ(passed_count, 3);
}

Task<void> BarrierWaiterTask(AsyncBarrier& barrier, int& count) {
  co_await barrier.Wait();
  count++;
}

TEST(AsyncBarrierTest, BarrierResetsAfterPassing) {
  EventLoop loop;
  int count1 = 0;
  int count2 = 0;

  loop.CallSoon([&]() {
    auto barrier = std::make_shared<AsyncBarrier>(2);

    // First round.
    auto t1 = BarrierWaiterTask(*barrier, count1);
    auto t2 = BarrierWaiterTask(*barrier, count1);

    auto after_first = [&]() -> Task<void> {
      co_await t1;
      co_await t2;
      // Second round — barrier should reset.
      auto t3 = BarrierWaiterTask(*barrier, count2);
      auto t4 = BarrierWaiterTask(*barrier, count2);
      co_await t3;
      co_await t4;
    }();

    after_first.AddDoneCallback([&](Future<void>&) { loop.Stop(); });
  });

  loop.RunForever();
  EXPECT_EQ(count1, 2);
  EXPECT_EQ(count2, 2);
}

TEST(AsyncBarrierTest, AbortBreaksBarrier) {
  EventLoop loop;
  bool caught = false;

  loop.CallSoon([&]() {
    AsyncBarrier barrier(3);
    auto f = barrier.Wait();
    EXPECT_FALSE(f.Done());

    barrier.Abort();

    // Subsequent Wait() should throw.
    try {
      barrier.Wait();
    } catch (const BrokenBarrierError&) {
      caught = true;
    }

    // Existing waiters should be broken.
    EXPECT_TRUE(f.Done());
    try {
      f.Result();
    } catch (const BrokenBarrierError&) {
      // Expected.
    }

    loop.Stop();
  });

  loop.RunForever();
  EXPECT_TRUE(caught);
}

TEST(AsyncBarrierTest, AbortBeforeAnyWaiters) {
  AsyncBarrier barrier(2);
  barrier.Abort();

  EXPECT_THROW({ barrier.Wait(); }, BrokenBarrierError);
}

TEST(AsyncBarrierTest, ResetAfterAbort) {
  AsyncBarrier barrier(2);
  barrier.Abort();

  // Reset should clear the broken state.
  barrier.Reset();

  // Now Wait() should work again.
  EventLoop loop;
  int count = 0;

  loop.CallSoon([&]() {
    auto t1 = BarrierWaiterTask(barrier, count);
    auto t2 = BarrierWaiterTask(barrier, count);

    auto check = [&]() -> Task<void> {
      co_await t1;
      co_await t2;
    }();

    check.AddDoneCallback([&](Future<void>&) { loop.Stop(); });
  });

  loop.RunForever();
  EXPECT_EQ(count, 2);
}

TEST(AsyncBarrierTest, ResetWhileWaitingAbortsWaiters) {
  EventLoop loop;
  bool caught = false;

  loop.CallSoon([&]() {
    auto barrier = std::make_shared<AsyncBarrier>(3);
    auto f = barrier->Wait();

    barrier->Reset();  // Should abort the pending waiter.
    EXPECT_TRUE(f.Done());
    try {
      f.Result();
    } catch (const BrokenBarrierError&) {
      caught = true;
    }
    loop.Stop();
  });

  loop.RunForever();
  EXPECT_TRUE(caught);
}

}  // namespace
}  // namespace asyncio
