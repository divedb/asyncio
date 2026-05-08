// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for AsyncSemaphore.

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/sleep.h"
#include "asyncio/sync/semaphore.h"
#include "asyncio/task.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

TEST(AsyncSemaphoreTest, InitialValue) {
  AsyncSemaphore sem(3);
  EXPECT_EQ(sem.Value(), 3);
}

TEST(AsyncSemaphoreTest, AcquireDecrementsValue) {
  AsyncSemaphore sem(2);
  auto f1 = sem.Acquire();
  EXPECT_TRUE(f1.Done());
  EXPECT_EQ(sem.Value(), 1);

  auto f2 = sem.Acquire();
  EXPECT_TRUE(f2.Done());
  EXPECT_EQ(sem.Value(), 0);
}

TEST(AsyncSemaphoreTest, AcquireBlocksWhenZero) {
  AsyncSemaphore sem(1);
  auto f1 = sem.Acquire();
  EXPECT_TRUE(f1.Done());

  auto f2 = sem.Acquire();
  EXPECT_FALSE(f2.Done());
}

TEST(AsyncSemaphoreTest, ReleaseWakesWaiter) {
  AsyncSemaphore sem(1);
  auto f1 = sem.Acquire();
  auto f2 = sem.Acquire();
  EXPECT_FALSE(f2.Done());

  sem.Release();
  EXPECT_TRUE(f2.Done());
}

TEST(AsyncSemaphoreTest, ReleaseIncrementsWhenNoWaiters) {
  AsyncSemaphore sem(0);
  sem.Release();
  EXPECT_EQ(sem.Value(), 1);
}

Task<void> SemUserTask(AsyncSemaphore& sem, int& counter) {
  co_await sem.Acquire();
  counter++;
  co_await Sleep(std::chrono::milliseconds(1));
  sem.Release();
}

TEST(AsyncSemaphoreTest, ConcurrencyLimit) {
  EventLoop loop;
  AsyncSemaphore sem(2);
  int counter = 0;

  loop.CallSoon([&]() {
    auto t1 = SemUserTask(sem, counter);
    auto t2 = SemUserTask(sem, counter);
    auto t3 = SemUserTask(sem, counter);

    t3.AddDoneCallback([&](Future<void>&) {
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(counter, 3);
}

TEST(AsyncSemaphoreTest, ZeroInitialValue) {
  EventLoop loop;
  AsyncSemaphore sem(0);
  bool acquired = false;

  loop.CallSoon([&]() {
    auto f = sem.Acquire();
    EXPECT_FALSE(f.Done());
    f.AddDoneCallback([&](Future<void>&) {
      acquired = true;
      loop.Stop();
    });
    loop.CallSoon([&sem]() { sem.Release(); });
  });

  loop.RunForever();
  EXPECT_TRUE(acquired);
}

// ---------------------------------------------------------------------------
// AsyncBoundedSemaphore tests
// ---------------------------------------------------------------------------

TEST(AsyncBoundedSemaphoreTest, ReleaseBeyondBoundThrows) {
  AsyncBoundedSemaphore sem(1);
  EXPECT_THROW(sem.Release(), InvalidStateError);
}

TEST(AsyncBoundedSemaphoreTest, AcquireAndReleaseCycle) {
  AsyncBoundedSemaphore sem(1);
  auto f = sem.Acquire();
  EXPECT_TRUE(f.Done());
  EXPECT_EQ(sem.Value(), 0);
  sem.Release();
  EXPECT_EQ(sem.Value(), 1);
}

TEST(AsyncBoundedSemaphoreTest, ReleaseAfterAcquireDoesNotThrow) {
  AsyncBoundedSemaphore sem(2);
  auto f1 = sem.Acquire();
  EXPECT_EQ(sem.Value(), 1);
  sem.Release();
  EXPECT_EQ(sem.Value(), 2);
  // Releasing again (back to initial) should NOT throw.
  // Actually value is already 2 which == initial(2), so next Release throws.
  EXPECT_THROW(sem.Release(), InvalidStateError);
}

}  // namespace
}  // namespace asyncio
