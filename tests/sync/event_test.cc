// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for AsyncEvent.

#include <memory>

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/sleep.h"
#include "asyncio/sync/event.h"
#include "asyncio/task.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

TEST(AsyncEventTest, InitiallyNotSet) {
  AsyncEvent event;
  EXPECT_FALSE(event.IsSet());
}

TEST(AsyncEventTest, SetAndIsSet) {
  AsyncEvent event;
  event.Set();
  EXPECT_TRUE(event.IsSet());
}

TEST(AsyncEventTest, ClearAfterSet) {
  AsyncEvent event;
  event.Set();
  EXPECT_TRUE(event.IsSet());
  event.Clear();
  EXPECT_FALSE(event.IsSet());
}

TEST(AsyncEventTest, WaitReturnsImmediatelyWhenSet) {
  EventLoop loop;
  bool done = false;

  loop.CallSoon([&]() {
    AsyncEvent event;
    event.Set();

    auto waiter = event.Wait();
    EXPECT_TRUE(waiter.Done());

    waiter.AddDoneCallback([&](Future<void>&) {
      done = true;
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(done);
}

TEST(AsyncEventTest, WaitBlocksUntilSet) {
  EventLoop loop;
  bool done = false;
  auto event = std::make_shared<AsyncEvent>();

  loop.CallSoon([&]() {
    auto waiter = event->Wait();
    EXPECT_FALSE(waiter.Done());

    waiter.AddDoneCallback([&](Future<void>&) {
      done = true;
      loop.Stop();
    });

    loop.CallSoon([event]() { event->Set(); });
  });

  loop.RunForever();
  EXPECT_TRUE(done);
}

TEST(AsyncEventTest, SetWakesMultipleWaiters) {
  EventLoop loop;
  int count = 0;
  auto event = std::make_shared<AsyncEvent>();

  loop.CallSoon([&]() {
    auto w1 = event->Wait();
    auto w2 = event->Wait();
    auto w3 = event->Wait();

    w1.AddDoneCallback([&](Future<void>&) {
      count++;
      if (count == 3) loop.Stop();
    });
    w2.AddDoneCallback([&](Future<void>&) {
      count++;
      if (count == 3) loop.Stop();
    });
    w3.AddDoneCallback([&](Future<void>&) {
      count++;
      if (count == 3) loop.Stop();
    });

    loop.CallSoon([event]() { event->Set(); });
  });

  loop.RunForever();
  EXPECT_EQ(count, 3);
}

Task<void> EventWaiterTask(std::shared_ptr<AsyncEvent> event, int& counter) {
  co_await event->Wait();
  counter++;
}

TEST(AsyncEventTest, WaitInCoroutine) {
  EventLoop loop;
  int counter = 0;
  auto event = std::make_shared<AsyncEvent>();

  loop.CallSoon([&]() {
    auto t1 = EventWaiterTask(event, counter);
    auto t2 = EventWaiterTask(event, counter);

    t1.AddDoneCallback([&](Future<void>&) {
      if (counter == 2) loop.Stop();
    });
    t2.AddDoneCallback([&](Future<void>&) {
      if (counter == 2) loop.Stop();
    });

    loop.CallSoon([event]() { event->Set(); });
  });

  loop.RunForever();
  EXPECT_EQ(counter, 2);
}

}  // namespace
}  // namespace asyncio
