// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for TimerHeap.

#include <chrono>
#include <vector>

#include "asyncio/detail/timer_heap.h"
#include "gtest/gtest.h"

namespace asyncio::detail {
namespace {

TEST(TimerHeapTest, EmptyOnInit) {
  TimerHeap heap;
  EXPECT_TRUE(heap.Empty());
  EXPECT_EQ(heap.Size(), 0);
}

TEST(TimerHeapTest, PushAndPopSingle) {
  auto when = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  int counter = 0;
  TimerHeap heap;
  heap.Push(TimerHandle(when, [&counter]() { counter++; }));

  EXPECT_FALSE(heap.Empty());
  EXPECT_EQ(heap.Size(), 1);

  TimerHandle handle = heap.Pop();
  EXPECT_EQ(handle.When(), when);
  EXPECT_TRUE(heap.Empty());
}

TEST(TimerHeapTest, MinHeapOrdering) {
  auto now = std::chrono::steady_clock::now();
  auto t1 = now + std::chrono::milliseconds(100);
  auto t2 = now + std::chrono::milliseconds(50);
  auto t3 = now + std::chrono::milliseconds(200);

  TimerHeap heap;
  heap.Push(TimerHandle(t1, []() {}));
  heap.Push(TimerHandle(t2, []() {}));
  heap.Push(TimerHandle(t3, []() {}));

  // Should pop in order: t2, t1, t3.
  EXPECT_EQ(heap.Pop().When(), t2);
  EXPECT_EQ(heap.Pop().When(), t1);
  EXPECT_EQ(heap.Pop().When(), t3);
}

TEST(TimerHeapTest, TopReturnsEarliest) {
  auto now = std::chrono::steady_clock::now();
  auto t1 = now + std::chrono::milliseconds(100);
  auto t2 = now + std::chrono::milliseconds(50);

  TimerHeap heap;
  heap.Push(TimerHandle(t1, []() {}));
  heap.Push(TimerHandle(t2, []() {}));

  EXPECT_EQ(heap.Top().When(), t2);
}

TEST(TimerHeapTest, PopSkipsCancelledHandles) {
  auto now = std::chrono::steady_clock::now();
  auto t1 = now + std::chrono::milliseconds(100);
  auto t2 = now + std::chrono::milliseconds(200);
  auto t3 = now + std::chrono::milliseconds(300);

  TimerHeap heap;
  TimerHandle h1(t1, []() {});
  TimerHandle h1_copy = h1;  // Keep a copy for cancellation.
  TimerHandle h2(t2, []() {});
  TimerHandle h3(t3, []() {});

  heap.Push(std::move(h1));
  heap.Push(std::move(h2));
  heap.Push(std::move(h3));

  // Cancel h1.
  h1_copy.Cancel();
  heap.NotifyCancelled();

  // Pop should skip h1 and return h2.
  EXPECT_EQ(heap.Pop().When(), t2);
  EXPECT_EQ(heap.Pop().When(), t3);
  EXPECT_TRUE(heap.Empty());
}

TEST(TimerHeapTest, MaybeRebuildCleansCancelled) {
  auto now = std::chrono::steady_clock::now();
  TimerHeap heap;

  // Push 110 handles, cancel 60 of them (>50%).
  std::vector<TimerHandle> handles;
  for (int i = 0; i < 110; ++i) {
    auto when = now + std::chrono::milliseconds(i);
    handles.push_back(TimerHandle(when, []() {}));
  }

  for (auto& h : handles) {
    heap.Push(h);
  }

  // Cancel first 60.
  for (int i = 0; i < 60; ++i) {
    handles[i].Cancel();
    heap.NotifyCancelled();
  }

  EXPECT_EQ(heap.Size(), 110);

  // Trigger rebuild.
  heap.MaybeRebuild();

  // After rebuild, only 50 entries remain.
  EXPECT_EQ(heap.Size(), 50);

  // Verify ordering of remaining entries.
  auto prev = heap.Pop().When();
  for (int i = 1; i < 50; ++i) {
    auto current = heap.Pop().When();
    EXPECT_LE(prev, current);
    prev = current;
  }
}

TEST(TimerHeapTest, MaybeRebuildNoOpWhenSmall) {
  auto now = std::chrono::steady_clock::now();
  TimerHeap heap;

  // Push 10 handles, cancel 8 (>50%). But size <= 100 so no rebuild.
  std::vector<TimerHandle> handles;
  for (int i = 0; i < 10; ++i) {
    handles.push_back(
        TimerHandle(now + std::chrono::milliseconds(i), []() {}));
  }
  for (auto& h : handles) {
    heap.Push(h);
  }
  for (int i = 0; i < 8; ++i) {
    handles[i].Cancel();
    heap.NotifyCancelled();
  }

  heap.MaybeRebuild();
  EXPECT_EQ(heap.Size(), 10);  // Not rebuilt — still contains all entries.
}

TEST(TimerHeapTest, ClearRemovesAll) {
  auto now = std::chrono::steady_clock::now();
  TimerHeap heap;
  heap.Push(TimerHandle(now + std::chrono::milliseconds(1), []() {}));
  heap.Push(TimerHandle(now + std::chrono::milliseconds(2), []() {}));
  heap.Clear();
  EXPECT_TRUE(heap.Empty());
}

}  // namespace
}  // namespace asyncio::detail
