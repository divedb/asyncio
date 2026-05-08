// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for AsyncCondition.

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/sleep.h"
#include "asyncio/sync/condition.h"
#include "asyncio/sync/lock.h"
#include "asyncio/task.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

Task<void> ConditionWaiterTask(AsyncLock& lock, AsyncCondition& cond,
                                bool& woken) {
  co_await lock.Acquire();
  co_await cond.Wait();
  woken = true;
  lock.Release();
}

Task<void> ConditionNotifierTask(AsyncLock& lock, AsyncCondition& cond) {
  co_await Sleep(std::chrono::milliseconds(5));
  co_await lock.Acquire();
  cond.Notify();
  lock.Release();
}

Task<void> ConditionNotifierAllTask(AsyncLock& lock, AsyncCondition& cond,
                                     int count) {
  co_await Sleep(std::chrono::milliseconds(5));
  co_await lock.Acquire();
  cond.Notify(count);
  lock.Release();
}

Task<void> ConditionAllDoneTask(Task<void> t1, Task<void> t2) {
  co_await t1;
  co_await t2;
}

TEST(AsyncConditionTest, NotifyWakesOneWaiter) {
  EventLoop loop;
  AsyncLock lock;
  AsyncCondition cond(lock);
  bool woken = false;

  loop.CallSoon([&]() {
    auto t1 = ConditionWaiterTask(lock, cond, woken);
    auto t2 = ConditionNotifierTask(lock, cond);

    t1.AddDoneCallback([&](Future<void>&) {
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(woken);
}

TEST(AsyncConditionTest, NotifyWakesSecondWaiter) {
  EventLoop loop;
  AsyncLock lock;
  AsyncCondition cond(lock);
  bool woken1 = false;
  bool woken2 = false;

  loop.CallSoon([&]() {
    auto t1 = ConditionWaiterTask(lock, cond, woken1);
    auto t2 = ConditionWaiterTask(lock, cond, woken2);

    auto notifier = ConditionNotifierAllTask(lock, cond, 2);
    auto done = ConditionAllDoneTask(t1, t2);

    done.AddDoneCallback([&](Future<void>&) {
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(woken1);
  EXPECT_TRUE(woken2);
}

Task<void> ConditionProducerTask(AsyncLock& lock, AsyncCondition& cond,
                                  int& value) {
  co_await Sleep(std::chrono::milliseconds(5));
  co_await lock.Acquire();
  value = 42;
  cond.Notify();
  lock.Release();
}

Task<int> ConditionConsumerTask(AsyncLock& lock, AsyncCondition& cond,
                                 int& value) {
  co_await lock.Acquire();
  while (value == 0) {
    co_await cond.Wait();
  }
  int result = value;
  lock.Release();
  co_return result;
}

TEST(AsyncConditionTest, ProducerConsumer) {
  EventLoop loop;
  AsyncLock lock;
  AsyncCondition cond(lock);
  int value = 0;
  int result = 0;

  loop.CallSoon([&]() {
    auto consumer = ConditionConsumerTask(lock, cond, value);
    auto producer = ConditionProducerTask(lock, cond, value);

    consumer.AddDoneCallback([&](Future<int>& f) {
      result = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(result, 42);
}

TEST(AsyncConditionTest, AcquireReleaseProxy) {
  EventLoop loop;
  AsyncLock lock;
  AsyncCondition cond(lock);

  loop.CallSoon([&]() {
    EXPECT_FALSE(cond.Locked());
    auto f = cond.Acquire();
    EXPECT_TRUE(f.Done());
    EXPECT_TRUE(cond.Locked());
    cond.Release();
    EXPECT_FALSE(cond.Locked());
    loop.Stop();
  });

  loop.RunForever();
}

}  // namespace
}  // namespace asyncio
