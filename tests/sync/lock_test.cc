// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for AsyncLock.

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/sleep.h"
#include "asyncio/sync/lock.h"
#include "asyncio/task.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

TEST(AsyncLockTest, InitiallyUnlocked) {
  AsyncLock lock;
  EXPECT_FALSE(lock.Locked());
}

TEST(AsyncLockTest, AcquireLocksImmediately) {
  EventLoop loop;
  AsyncLock lock;
  bool locked = false;

  loop.CallSoon([&]() {
    auto f = lock.Acquire();
    EXPECT_TRUE(f.Done());
    locked = lock.Locked();
    loop.Stop();
  });

  loop.RunForever();
  EXPECT_TRUE(locked);
}

TEST(AsyncLockTest, ReleaseUnlocks) {
  EventLoop loop;
  AsyncLock lock;

  loop.CallSoon([&]() {
    auto f = lock.Acquire();
    EXPECT_TRUE(lock.Locked());
    lock.Release();
    EXPECT_FALSE(lock.Locked());
    loop.Stop();
  });

  loop.RunForever();
}

TEST(AsyncLockTest, ReleaseUnlockedThrows) {
  AsyncLock lock;
  EXPECT_THROW(lock.Release(), InvalidStateError);
}

TEST(AsyncLockTest, ContentionWakesWaiter) {
  EventLoop loop;
  AsyncLock lock;
  int order = 0;
  int first = 0;
  int second = 0;

  loop.CallSoon([&]() {
    // First coroutine grabs the lock.
    auto f1 = lock.Acquire();
    EXPECT_TRUE(f1.Done());

    // Second coroutine waits.
    auto f2 = lock.Acquire();
    EXPECT_FALSE(f2.Done());

    f2.AddDoneCallback([&](Future<void>&) {
      second = ++order;
      lock.Release();
      loop.Stop();
    });

    first = ++order;
    lock.Release();
  });

  loop.RunForever();
  EXPECT_EQ(first, 1);
  EXPECT_EQ(second, 2);
}

Task<void> LockUserTask(AsyncLock& lock, int& counter) {
  co_await lock.Acquire();
  int val = counter;
  co_await Sleep(std::chrono::milliseconds(1));
  counter = val + 1;
  lock.Release();
}

TEST(AsyncLockTest, MutualExclusionWithTasks) {
  EventLoop loop;
  AsyncLock lock;
  int counter = 0;

  loop.CallSoon([&]() {
    auto t1 = LockUserTask(lock, counter);
    auto t2 = LockUserTask(lock, counter);

    t2.AddDoneCallback([&](Future<void>&) {
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(counter, 2);
}

TEST(AsyncLockTest, LockGuardReleasesOnDestruction) {
  EventLoop loop;
  AsyncLock lock;

  loop.CallSoon([&]() {
    {
      auto f = lock.Acquire();
      AsyncLock::LockGuard guard(lock);
      EXPECT_TRUE(lock.Locked());
    }
    EXPECT_FALSE(lock.Locked());
    loop.Stop();
  });

  loop.RunForever();
}

TEST(AsyncLockTest, CancelledWaiterSkipped) {
  EventLoop loop;
  AsyncLock lock;

  loop.CallSoon([&]() {
    // Lock is held.
    auto f1 = lock.Acquire();
    EXPECT_TRUE(lock.Locked());

    // Two waiters.
    auto f2 = lock.Acquire();
    auto f3 = lock.Acquire();

    // Cancel the first waiter.
    f2.Cancel();

    // Release the lock — should skip f2 and wake f3.
    lock.Release();

    EXPECT_TRUE(f3.Done());
    EXPECT_TRUE(lock.Locked());

    lock.Release();
    loop.Stop();
  });

  loop.RunForever();
}

}  // namespace
}  // namespace asyncio
