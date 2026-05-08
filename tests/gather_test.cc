// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for Gather().

#include <chrono>

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/gather.h"
#include "asyncio/sleep.h"
#include "asyncio/task.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

// --- Helpers ---

Task<int> DelayedInt(int val, std::chrono::nanoseconds delay) {
  co_await Sleep(delay);
  co_return val;
}

Task<void> DelayedVoid(std::chrono::nanoseconds delay) {
  co_await Sleep(delay);
}

// --- Vector Gather (non-void) ---

TEST(GatherTest, EmptyGather) {
  EventLoop loop;
  std::vector<int> results;

  loop.CallSoon([&]() {
    std::vector<Future<int>> futures;
    auto gather = Gather(std::move(futures));
    gather.AddDoneCallback([&](Future<std::vector<int>>& f) {
      results = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(results.empty());
}

TEST(GatherTest, SingleFuture) {
  EventLoop loop;
  std::vector<int> results;

  loop.CallSoon([&]() {
    std::vector<Future<int>> futures;
    futures.push_back(DelayedInt(42, std::chrono::milliseconds(1)));
    auto gather = Gather(std::move(futures));
    gather.AddDoneCallback([&](Future<std::vector<int>>& f) {
      results = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0], 42);
}

TEST(GatherTest, MultipleFutures) {
  EventLoop loop;
  std::vector<int> results;

  loop.CallSoon([&]() {
    std::vector<Future<int>> futures;
    futures.push_back(DelayedInt(1, std::chrono::milliseconds(20)));
    futures.push_back(DelayedInt(2, std::chrono::milliseconds(10)));
    futures.push_back(DelayedInt(3, std::chrono::milliseconds(5)));

    auto gather = Gather(std::move(futures));
    gather.AddDoneCallback([&](Future<std::vector<int>>& f) {
      results = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  ASSERT_EQ(results.size(), 3u);
  // Results are in input order, not completion order.
  EXPECT_EQ(results[0], 1);
  EXPECT_EQ(results[1], 2);
  EXPECT_EQ(results[2], 3);
}

TEST(GatherTest, ExceptionPropagates) {
  EventLoop loop;
  bool caught = false;

  loop.CallSoon([&]() {
    std::vector<Future<int>> futures;
    futures.push_back(DelayedInt(1, std::chrono::milliseconds(1)));

    Task<int> thrower = []() -> Task<int> {
      co_await Sleep(std::chrono::milliseconds(1));
      throw std::runtime_error("gather_err");
      co_return 0;
    }();
    futures.push_back(std::move(thrower));

    futures.push_back(DelayedInt(3, std::chrono::milliseconds(1)));

    auto gather = Gather(std::move(futures));
    gather.AddDoneCallback([&](Future<std::vector<int>>& f) {
      try {
        f.Result();
      } catch (const std::runtime_error& e) {
        caught = std::string(e.what()) == "gather_err";
      }
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_TRUE(caught);
}

// --- Vector Gather (void) ---

TEST(GatherTest, VoidFutures) {
  EventLoop loop;
  int count = 0;

  loop.CallSoon([&]() {
    std::vector<Future<void>> futures;
    futures.push_back(DelayedVoid(std::chrono::milliseconds(1)));
    futures.push_back(DelayedVoid(std::chrono::milliseconds(1)));

    auto gather = Gather(std::move(futures));
    gather.AddDoneCallback([&](Future<void>&) {
      count = 1;
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(count, 1);
}

// --- Variadic Gather ---

TEST(GatherTest, VariadicGather) {
  EventLoop loop;
  std::tuple<int, int, int> result;

  loop.CallSoon([&]() {
    auto gather = Gather(
        DelayedInt(10, std::chrono::milliseconds(1)),
        DelayedInt(20, std::chrono::milliseconds(1)),
        DelayedInt(30, std::chrono::milliseconds(1)));

    gather.AddDoneCallback([&](Future<std::tuple<int, int, int>>& f) {
      result = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  EXPECT_EQ(std::get<0>(result), 10);
  EXPECT_EQ(std::get<1>(result), 20);
  EXPECT_EQ(std::get<2>(result), 30);
}

// --- GatherWithExceptions ---

TEST(GatherTest, GatherWithExceptions) {
  EventLoop loop;
  using Entry = std::variant<int, std::exception_ptr>;
  std::vector<Entry> results;

  loop.CallSoon([&]() {
    std::vector<Future<int>> futures;
    futures.push_back(DelayedInt(1, std::chrono::milliseconds(1)));

    Task<int> thrower = []() -> Task<int> {
      co_await Sleep(std::chrono::milliseconds(1));
      throw std::runtime_error("exc");
      co_return 0;
    }();
    futures.push_back(std::move(thrower));

    auto gather = GatherWithExceptions(std::move(futures));
    gather.AddDoneCallback([&](Future<std::vector<Entry>>& f) {
      results = f.Result();
      loop.Stop();
    });
  });

  loop.RunForever();
  ASSERT_EQ(results.size(), 2u);

  // First should be a value.
  ASSERT_TRUE(std::holds_alternative<int>(results[0]));
  EXPECT_EQ(std::get<int>(results[0]), 1);

  // Second should be an exception.
  ASSERT_TRUE(std::holds_alternative<std::exception_ptr>(results[1]));
  try {
    std::rethrow_exception(std::get<std::exception_ptr>(results[1]));
  } catch (const std::runtime_error& e) {
    EXPECT_STREQ(e.what(), "exc");
  }
}

}  // namespace
}  // namespace asyncio
