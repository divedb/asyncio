// Copyright 2025 asyncio-cpp authors. All rights reserved.
// queue_ext_test.cc — Tests for extended queue functionality.

#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "asyncio/error.h"
#include "asyncio/sync/priority_queue.h"
#include "asyncio/sync/lifo_queue.h"
#include "asyncio/sync/queue.h"

namespace {

using namespace asyncio;

// ============================================================================
// AsyncQueue PutNowait/GetNowait tests
// ============================================================================

TEST(AsyncQueueNowaitTest, PutNowaitOnUnboundedQueue) {
  AsyncQueue<int> q;
  q.PutNowait(1);
  q.PutNowait(2);
  EXPECT_EQ(q.Size(), 2u);
  EXPECT_EQ(q.GetNowait(), 1);
  EXPECT_EQ(q.GetNowait(), 2);
}

TEST(AsyncQueueNowaitTest, PutNowaitOnBoundedQueue) {
  AsyncQueue<int> q(2);
  q.PutNowait(1);
  q.PutNowait(2);
  EXPECT_THROW(q.PutNowait(3), QueueFullError);
}

TEST(AsyncQueueNowaitTest, GetNowaitOnEmptyQueue) {
  AsyncQueue<int> q;
  EXPECT_THROW(q.GetNowait(), QueueEmptyError);
}

TEST(AsyncQueueNowaitTest, PutNowaitAfterShutdown) {
  AsyncQueue<int> q;
  q.Shutdown();
  EXPECT_THROW(q.PutNowait(1), QueueShutDownError);
}

TEST(AsyncQueueNowaitTest, GetNowaitAfterShutdown) {
  AsyncQueue<int> q;
  q.PutNowait(1);
  q.Shutdown();
  EXPECT_EQ(q.GetNowait(), 1);
  EXPECT_THROW(q.GetNowait(), QueueShutDownError);
}

// ============================================================================
// AsyncPriorityQueue tests
// ============================================================================

TEST(AsyncPriorityQueueTest, PutNowaitAndGetNowait) {
  AsyncPriorityQueue<int> q;

  // Lower priority value = higher priority
  q.PutNowait(100, 2);  // Low priority
  q.PutNowait(10, 0);   // High priority
  q.PutNowait(50, 1);   // Medium priority

  // Should come out in priority order
  auto [item1, pri1] = q.GetNowait();
  EXPECT_EQ(item1, 10);
  EXPECT_EQ(pri1, 0);

  auto [item2, pri2] = q.GetNowait();
  EXPECT_EQ(item2, 50);
  EXPECT_EQ(pri2, 1);

  auto [item3, pri3] = q.GetNowait();
  EXPECT_EQ(item3, 100);
  EXPECT_EQ(pri3, 2);
}

TEST(AsyncPriorityQueueTest, BoundedQueue) {
  AsyncPriorityQueue<int> q(2);

  q.PutNowait(1, 1);
  q.PutNowait(2, 0);

  EXPECT_THROW(q.PutNowait(3, 0), QueueFullError);
  EXPECT_EQ(q.Size(), 2u);
}

TEST(AsyncPriorityQueueTest, EmptyQueue) {
  AsyncPriorityQueue<int> q;
  EXPECT_TRUE(q.Empty());
  EXPECT_THROW(q.GetNowait(), QueueEmptyError);
}

TEST(AsyncPriorityQueueTest, Shutdown) {
  AsyncPriorityQueue<int> q;
  q.Shutdown();
  EXPECT_THROW(q.PutNowait(1, 0), QueueShutDownError);
  EXPECT_THROW(q.GetNowait(), QueueShutDownError);
}

// ============================================================================
// AsyncLifoQueue tests
// ============================================================================

TEST(AsyncLifoQueueTest, PutNowaitAndGetNowait) {
  AsyncLifoQueue<int> q;

  q.PutNowait(1);
  q.PutNowait(2);
  q.PutNowait(3);

  // LIFO: last in, first out
  EXPECT_EQ(q.GetNowait(), 3);
  EXPECT_EQ(q.GetNowait(), 2);
  EXPECT_EQ(q.GetNowait(), 1);
}

TEST(AsyncLifoQueueTest, BoundedQueue) {
  AsyncLifoQueue<int> q(2);

  q.PutNowait(1);
  q.PutNowait(2);

  EXPECT_THROW(q.PutNowait(3), QueueFullError);
  EXPECT_EQ(q.Size(), 2u);
}

TEST(AsyncLifoQueueTest, EmptyQueue) {
  AsyncLifoQueue<std::string> q;
  EXPECT_TRUE(q.Empty());
  EXPECT_THROW(q.GetNowait(), QueueEmptyError);
}

TEST(AsyncLifoQueueTest, Shutdown) {
  AsyncLifoQueue<int> q;
  q.Shutdown();
  EXPECT_THROW(q.PutNowait(1), QueueShutDownError);
  EXPECT_THROW(q.GetNowait(), QueueShutDownError);
}

// ============================================================================
// AsyncLifoQueue with complex types
// ============================================================================

TEST(AsyncLifoQueueTest, WithMoveOnlyType) {
  AsyncLifoQueue<std::unique_ptr<int>> q;

  q.PutNowait(std::make_unique<int>(42));
  q.PutNowait(std::make_unique<int>(100));

  auto ptr1 = q.GetNowait();
  EXPECT_EQ(*ptr1, 100);  // Last in, first out

  auto ptr2 = q.GetNowait();
  EXPECT_EQ(*ptr2, 42);
}

// ============================================================================
// PriorityQueue with complex types
// ============================================================================

TEST(AsyncPriorityQueueTest, WithPairs) {
  AsyncPriorityQueue<std::pair<int, std::string>> q;

  // Priority queue of tasks: (priority, name)
  q.PutNowait({1, "medium"}, 1);
  q.PutNowait({3, "high"}, 0);
  q.PutNowait({2, "low"}, 2);

  auto [val1, pri1] = q.GetNowait();
  EXPECT_EQ(val1.first, 3);
  EXPECT_EQ(val1.second, "high");
  EXPECT_EQ(pri1, 0);
}

}  // namespace
