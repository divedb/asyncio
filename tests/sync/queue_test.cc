// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for AsyncQueue<T>.

#include <memory>

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/sleep.h"
#include "asyncio/sync/queue.h"
#include "asyncio/task.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

// --- Helper tasks (parameters are stored in coroutine frame, not closure) ---

Task<void> SimplePutTask(std::shared_ptr<AsyncQueue<int>> q, int val) {
  co_await q->Put(val);
}

Task<int> SimpleGetTask(std::shared_ptr<AsyncQueue<int>> q) {
  co_return co_await q->Get();
}

Task<void> ProducerTask(std::shared_ptr<AsyncQueue<int>> q, int count) {
  for (int i = 0; i < count; ++i) {
    co_await q->Put(i);
  }
}

Task<int> ConsumerTask(std::shared_ptr<AsyncQueue<int>> q, int count) {
  int sum = 0;
  for (int i = 0; i < count; ++i) {
    sum += co_await q->Get();
  }
  co_return sum;
}

Task<void> DelayedPutTask(std::shared_ptr<AsyncQueue<int>> q, int val) {
  co_await Sleep(std::chrono::milliseconds(5));
  co_await q->Put(val);
}

Task<int> BoundedDrainTask(std::shared_ptr<AsyncQueue<int>> q) {
  co_await Sleep(std::chrono::milliseconds(5));
  int a = co_await q->Get();
  int b = co_await q->Get();
  co_return a + b;
}

Task<void> BoundedFillTask(std::shared_ptr<AsyncQueue<int>> q,
                            bool& put_completed) {
  co_await q->Put(1);
  co_await q->Put(2);  // Blocks when full.
  put_completed = true;
}

Task<void> SetupPutGetTask(std::shared_ptr<AsyncQueue<int>> q, int& result) {
  co_await q->Put(10);
  co_await q->Put(20);
  result = co_await q->Get();
}

// --- Tests ---

TEST(AsyncQueueTest, InitiallyEmpty) {
  AsyncQueue<int> q;
  EXPECT_TRUE(q.Empty());
  EXPECT_EQ(q.Size(), 0u);
  EXPECT_FALSE(q.Full());
}

TEST(AsyncQueueTest, PutAndGet) {
  EventLoop loop;
  int result = 0;
  auto q = std::make_shared<AsyncQueue<int>>();

  loop.CallSoon([&]() {
    auto t = [&]() -> Task<void> {
      // Use function parameters, not lambda captures, for coroutine locals.
      co_await SimplePutTask(q, 42);
      result = co_await SimpleGetTask(q);
    }();

    t.AddDoneCallback([&](Future<void>&) { loop.Stop(); });
  });

  loop.RunForever();
  EXPECT_EQ(result, 42);
}

TEST(AsyncQueueTest, ProducerConsumer) {
  EventLoop loop;
  int sum = 0;
  auto q = std::make_shared<AsyncQueue<int>>();

  loop.CallSoon([&]() {
    auto p = ProducerTask(q, 5);
    auto c = ConsumerTask(q, 5);

    c.AddDoneCallback([&](Future<int>& f) {
      sum = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(sum, 10);  // 0+1+2+3+4
}

TEST(AsyncQueueTest, ConsumerBlocksWhenEmpty) {
  EventLoop loop;
  int result = 0;
  auto q = std::make_shared<AsyncQueue<int>>();

  loop.CallSoon([&]() {
    auto consumer = SimpleGetTask(q);
    auto producer = DelayedPutTask(q, 99);

    consumer.AddDoneCallback([&](Future<int>& f) {
      result = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(result, 99);
}

TEST(AsyncQueueTest, TaskDoneAndJoin) {
  EventLoop loop;
  bool join_done = false;
  auto q = std::make_shared<AsyncQueue<int>>();

  loop.CallSoon([&]() {
    auto producer = [&]() -> Task<void> {
      co_await SimplePutTask(q, 1);
      co_await SimplePutTask(q, 2);
    }();

    auto consumer = [&]() -> Task<void> {
      co_await SimpleGetTask(q);
      q->TaskDone();
      co_await SimpleGetTask(q);
      q->TaskDone();
    }();

    auto joiner = [&]() -> Task<void> {
      co_await q->Join();
    }();

    auto check = [&]() -> Task<void> {
      co_await producer;
      co_await consumer;
      co_await joiner;
      join_done = true;
    }();

    check.AddDoneCallback([&](Future<void>&) { loop.Stop(); });
  });

  loop.RunForever();
  EXPECT_TRUE(join_done);
}

TEST(AsyncQueueTest, ShutdownWakesGetters) {
  EventLoop loop;
  bool caught = false;
  auto q = std::make_shared<AsyncQueue<int>>();

  loop.CallSoon([&]() {
    auto getter = SimpleGetTask(q);

    getter.AddDoneCallback([&](Future<int>& f) {
      try {
        f.Result();
      } catch (const QueueShutDownError&) {
        caught = true;
      }
      loop.Stop();
    });

    loop.CallSoon([q]() { q->Shutdown(); });
  });

  loop.RunForever();
  EXPECT_TRUE(caught);
}

TEST(AsyncQueueTest, ShutdownPreventsPut) {
  EventLoop loop;
  bool caught = false;
  auto q = std::make_shared<AsyncQueue<int>>();
  q->Shutdown();

  loop.CallSoon([&]() {
    auto t = SimplePutTask(q, 1);
    t.AddDoneCallback([&](Future<void>& f) {
      try {
        f.Result();
      } catch (const QueueShutDownError&) {
        caught = true;
      }
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(caught);
}

TEST(AsyncQueueTest, BoundedQueueBlocksPut) {
  EventLoop loop;
  bool put_completed = false;
  auto q = std::make_shared<AsyncQueue<int>>(1);

  loop.CallSoon([&]() {
    auto filler = BoundedFillTask(q, put_completed);
    auto drainer = BoundedDrainTask(q);

    drainer.AddDoneCallback([&](Future<int>& f) {
      EXPECT_EQ(f.Result(), 3);
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(put_completed);
}

TEST(AsyncQueueTest, GetExistingItems) {
  EventLoop loop;
  int result = 0;
  auto q = std::make_shared<AsyncQueue<int>>();

  loop.CallSoon([&]() {
    auto t = SetupPutGetTask(q, result);
    t.AddDoneCallback([&](Future<void>&) { loop.Stop(); });
  });

  loop.RunForever();
  EXPECT_EQ(result, 10);
}

}  // namespace
}  // namespace asyncio
