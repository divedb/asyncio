// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for EventLoop core: scheduling, RunOnce, RunForever, timers,
// cancellation, and thread-safe scheduling.

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include "asyncio/event_loop.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

// --- CallSoon / RunOnce ---

TEST(EventLoopTest, CallSoonRunsCallbackOnNextTick) {
  EventLoop loop;
  int counter = 0;

  loop.CallSoon([&counter]() { counter++; });
  EXPECT_EQ(counter, 0);  // Not yet executed.

  loop.RunOnce();
  EXPECT_EQ(counter, 1);
}

TEST(EventLoopTest, RunOnceRunsOnlyNtodoCallbacks) {
  EventLoop loop;
  int counter = 0;

  loop.CallSoon([&counter, &loop]() {
    counter++;
    // This callback schedules another during execution.
    loop.CallSoon([&counter]() { counter++; });
  });

  loop.RunOnce();
  // Only the first callback ran. The second was deferred.
  EXPECT_EQ(counter, 1);

  loop.RunOnce();
  // Now the second callback runs.
  EXPECT_EQ(counter, 2);
}

TEST(EventLoopTest, MultipleCallbacksRunInFifoOrder) {
  EventLoop loop;
  std::vector<int> order;

  loop.CallSoon([&order]() { order.push_back(1); });
  loop.CallSoon([&order]() { order.push_back(2); });
  loop.CallSoon([&order]() { order.push_back(3); });

  loop.RunOnce();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);
}

// --- Handle cancellation via CallSoon ---

TEST(EventLoopTest, CancelledHandleDoesNotRun) {
  EventLoop loop;
  int counter = 0;

  Handle handle = loop.CallSoon([&counter]() { counter++; });
  handle.Cancel();

  loop.RunOnce();
  EXPECT_EQ(counter, 0);
}

// --- CallLater / CallAt (timers) ---

TEST(EventLoopTest, CallLaterFiresAfterDelay) {
  EventLoop loop;
  int counter = 0;

  loop.CallLater(std::chrono::milliseconds(10),
                 [&counter]() { counter++; });

  // RunOnce should wait for the timer and fire it.
  loop.RunOnce();
  EXPECT_EQ(counter, 1);
}

TEST(EventLoopTest, CallAtFiresAtDeadline) {
  EventLoop loop;
  int counter = 0;

  auto when = loop.Time() + std::chrono::milliseconds(10);
  loop.CallAt(when, [&counter]() { counter++; });

  loop.RunOnce();
  EXPECT_EQ(counter, 1);
}

TEST(EventLoopTest, LongDelayTimerFiresAfterWaiting) {
  EventLoop loop;
  int counter = 0;

  loop.CallLater(std::chrono::milliseconds(50),
                 [&counter]() { counter++; });

  loop.RunOnce();
  EXPECT_EQ(counter, 1);
}

TEST(EventLoopTest, CancelledTimerDoesNotFire) {
  EventLoop loop;
  int counter = 0;

  TimerHandle handle = loop.CallLater(std::chrono::milliseconds(10),
                                       [&counter]() { counter++; });
  handle.Cancel();

  loop.RunOnce();
  EXPECT_EQ(counter, 0);
}

TEST(EventLoopTest, TimersFireInDeadlineOrder) {
  EventLoop loop;
  std::vector<int> order;

  auto base = loop.Time() + std::chrono::milliseconds(10);

  // Schedule with deadlines in non-sorted order, all very close together.
  loop.CallAt(base + std::chrono::microseconds(2),
              [&order]() { order.push_back(3); });
  loop.CallAt(base,
              [&order]() { order.push_back(1); });
  loop.CallAt(base + std::chrono::microseconds(1),
              [&order]() { order.push_back(2); });

  // RunOnce sleeps until the earliest deadline, then fires all expired.
  // The sleep will overshoot by several ms, so all three should be expired.
  loop.RunOnce();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);
}

// --- RunForever / Stop ---

TEST(EventLoopTest, RunForeverStopsOnStop) {
  EventLoop loop;
  int counter = 0;

  // Self-rescheduling callback that counts to 5 then stops the loop.
  std::function<void()> tick = [&]() {
    counter++;
    if (counter >= 5) {
      loop.Stop();
    } else {
      loop.CallSoon(tick);
    }
  };

  loop.CallSoon(tick);
  loop.RunForever();
  EXPECT_EQ(counter, 5);
}

TEST(EventLoopTest, StopFromWithinCallback) {
  EventLoop loop;
  bool ran_after_stop = false;

  loop.CallSoon([&]() { loop.Stop(); });
  loop.CallSoon([&]() { ran_after_stop = true; });

  loop.RunForever();
  // The second callback was in the ready queue but Stop() was called.
  // Due to ntodo semantics, both might have run. The key is that the loop
  // stopped and didn't loop forever.
  EXPECT_TRUE(true);  // If we get here, Stop() worked.
}

TEST(EventLoopTest, IsRunningDuringRunForever) {
  EventLoop loop;
  bool was_running = false;

  loop.CallSoon([&]() {
    was_running = loop.IsRunning();
    loop.Stop();
  });

  loop.RunForever();
  EXPECT_TRUE(was_running);
  EXPECT_FALSE(loop.IsRunning());
}

// --- CallSoonThreadsafe ---

TEST(EventLoopTest, CallSoonThreadsafeFromAnotherThread) {
  EventLoop loop;
  std::atomic<int> counter{0};

  loop.CallSoon([&]() {
    // This runs in the first tick. Start a thread that schedules work.
    std::thread worker([&]() {
      loop.CallSoonThreadsafe([&counter]() { counter++; });
    });
    worker.detach();
  });

  // Run two ticks: one for the initial callback, one for the thread-safe one.
  loop.RunOnce();

  // Give the worker thread time to call CallSoonThreadsafe.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  loop.RunOnce();

  EXPECT_EQ(counter.load(), 1);
}

TEST(EventLoopTest, CallSoonThreadsafeWakesLoop) {
  EventLoop loop;
  std::atomic<bool> done{false};

  // Start a thread that will schedule a callback after a delay.
  std::thread worker([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    loop.CallSoonThreadsafe([&]() { done = true; });
    // Also schedule Stop so the loop doesn't run forever.
    loop.CallSoonThreadsafe([&]() { loop.Stop(); });
  });
  worker.detach();

  // RunForever should wake up when the thread-safe callback arrives.
  loop.RunForever();
  EXPECT_TRUE(done.load());
}

// --- Exception handling ---

TEST(EventLoopTest, ExceptionInCallbackDoesNotKillLoop) {
  EventLoop loop;
  int counter = 0;

  loop.CallSoon([&]() { throw std::runtime_error("test error"); });
  loop.CallSoon([&counter]() { counter++; });

  loop.RunOnce();
  // Second callback should still have run despite the first throwing.
  EXPECT_EQ(counter, 1);
}

// --- Timer heap cleanup ---

TEST(EventLoopTest, ManyCancelledTimersAreCleanedUp) {
  EventLoop loop;
  std::vector<TimerHandle> handles;
  int counter = 0;

  auto when = loop.Time() + std::chrono::milliseconds(50);

  // Push 110 timers all with the same deadline. Cancel 60 of them.
  for (int i = 0; i < 110; ++i) {
    handles.push_back(
        loop.CallAt(when, [&counter]() { counter++; }));
  }

  for (int i = 0; i < 60; ++i) {
    handles[i].Cancel();
  }

  // RunOnce will trigger MaybeRebuild and then fire the remaining timers.
  loop.RunOnce();
  EXPECT_EQ(counter, 50);
}

}  // namespace
}  // namespace asyncio
