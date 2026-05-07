// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for Handle and TimerHandle.

#include <chrono>
#include <functional>

#include "asyncio/handle.h"
#include "asyncio/timer_handle.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

// --- Handle Tests ---

TEST(HandleTest, RunExecutesCallback) {
  int counter = 0;
  Handle handle([&counter]() { counter++; });
  handle.Run();
  EXPECT_EQ(counter, 1);
}

TEST(HandleTest, RunIsNoopAfterCancel) {
  int counter = 0;
  Handle handle([&counter]() { counter++; });
  EXPECT_TRUE(handle.Cancel());
  handle.Run();
  EXPECT_EQ(counter, 0);
}

TEST(HandleTest, CancelReturnsFalseIfAlreadyCancelled) {
  Handle handle([]() {});
  EXPECT_TRUE(handle.Cancel());
  EXPECT_FALSE(handle.Cancel());
}

TEST(HandleTest, CancelledReturnsCorrectState) {
  Handle handle([]() {});
  EXPECT_FALSE(handle.Cancelled());
  handle.Cancel();
  EXPECT_TRUE(handle.Cancelled());
}

TEST(HandleTest, DefaultConstructedIsInvalid) {
  Handle handle;
  EXPECT_FALSE(handle.Valid());
  // Cancelled() returns false for a null handle — there is nothing to cancel.
  EXPECT_FALSE(handle.Cancelled());
  // Run on a null handle is a no-op.
  handle.Run();
}

TEST(HandleTest, CopySharesState) {
  int counter = 0;
  Handle original([&counter]() { counter++; });
  Handle copy = original;

  // Cancelling the copy cancels the original.
  copy.Cancel();
  EXPECT_TRUE(original.Cancelled());
  EXPECT_TRUE(copy.Cancelled());

  // Running either is a no-op.
  original.Run();
  copy.Run();
  EXPECT_EQ(counter, 0);
}

TEST(HandleTest, CancelFromAnotherCopyPreventsRun) {
  int counter = 0;
  Handle h1([&counter]() { counter++; });
  Handle h2 = h1;

  h2.Cancel();
  h1.Run();
  EXPECT_EQ(counter, 0);
}

// --- TimerHandle Tests ---

TEST(TimerHandleTest, StoresDeadline) {
  auto when = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  TimerHandle handle(when, []() {});
  EXPECT_EQ(handle.When(), when);
}

TEST(TimerHandleTest, ComparisonOperators) {
  auto t1 = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
  auto t2 = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);

  TimerHandle earlier(t1, []() {});
  TimerHandle later(t2, []() {});

  EXPECT_TRUE(earlier < later);
  EXPECT_FALSE(later < earlier);
  EXPECT_TRUE(later > earlier);
  EXPECT_FALSE(earlier > later);
}

TEST(TimerHandleTest, InheritsCancellation) {
  auto when = std::chrono::steady_clock::now();
  int counter = 0;
  TimerHandle handle(when, [&counter]() { counter++; });

  handle.Cancel();
  EXPECT_TRUE(handle.Cancelled());
  handle.Run();
  EXPECT_EQ(counter, 0);
}

TEST(TimerHandleTest, CopySharesCancellation) {
  auto when = std::chrono::steady_clock::now();
  TimerHandle h1(when, []() {});
  TimerHandle h2 = h1;

  h2.Cancel();
  EXPECT_TRUE(h1.Cancelled());
}

TEST(TimerHandleTest, DefaultConstructedIsInvalid) {
  TimerHandle handle;
  EXPECT_FALSE(handle.Valid());
}

}  // namespace
}  // namespace asyncio
