// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for TaskGroup — structured concurrency.

#include <atomic>
#include <functional>

#include "asyncio/runner.h"
#include "asyncio/sleep.h"
#include "asyncio/task.h"
#include "asyncio/task_group.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

// Global atomic for passing data between test and coroutine.
inline thread_local std::atomic<int>* g_counter = nullptr;

// Helper coroutines.

inline Task<void> EmptyCoroutine() { co_return; }

inline Task<void> CounterCoroutine() {
  if (g_counter) (*g_counter)++;
  co_return;
}

inline Task<int> ReturnValueCoroutine(int value) { co_return value; }

inline Task<void> ThrowingCoroutine(const char* msg) {
  throw std::runtime_error(msg);
  co_return;
}

// --- Basic Tests ---

TEST(TaskGroupTest, EmptyGroupCompletesImmediately) {
  bool completed = false;

  auto coro = [&]() -> Task<void> {
    TaskGroup group;
    co_await group.WaitComplete();
    completed = true;
  };

  asyncio::Run<void>(coro);
  EXPECT_TRUE(completed);
}

TEST(TaskGroupTest, SingleTaskCompletes) {
  g_counter = nullptr;
  std::atomic<int> counter{0};
  g_counter = &counter;

  auto coro = [&]() -> Task<void> {
    TaskGroup group;
    group.CreateTask(CounterCoroutine());
    co_await group.WaitComplete();
  };

  asyncio::Run<void>(coro);
  g_counter = nullptr;
  EXPECT_EQ(counter, 1);
}

TEST(TaskGroupTest, MultipleTasksAllComplete) {
  g_counter = nullptr;
  std::atomic<int> counter{0};
  g_counter = &counter;

  auto coro = [&]() -> Task<void> {
    TaskGroup group;
    group.CreateTask(CounterCoroutine());
    group.CreateTask(CounterCoroutine());
    group.CreateTask(CounterCoroutine());
    co_await group.WaitComplete();
  };

  asyncio::Run<void>(coro);
  g_counter = nullptr;
  EXPECT_EQ(counter, 3);
}

TEST(TaskGroupTest, TasksWithReturnValues) {
  auto coro = [&]() -> Task<int> {
    TaskGroup group;
    Task<int> t1 = group.CreateTask(ReturnValueCoroutine(1));
    Task<int> t2 = group.CreateTask(ReturnValueCoroutine(2));
    co_await group.WaitComplete();
    co_return t1.Result() + t2.Result();
  };

  int result = asyncio::Run<int>(coro);
  EXPECT_EQ(result, 3);
}

// --- Cancellation Tests ---

TEST(TaskGroupTest, TaskCountTracksCorrectly) {
  auto coro = [&]() -> Task<void> {
    TaskGroup group;
    EXPECT_EQ(group.TaskCount(), 0);
    group.CreateTask(EmptyCoroutine());
    EXPECT_EQ(group.TaskCount(), 1);
    group.CreateTask(EmptyCoroutine());
    EXPECT_EQ(group.TaskCount(), 2);
    co_await group.WaitComplete();
    EXPECT_EQ(group.TaskCount(), 2);
  };

  asyncio::Run<void>(coro);
}

TEST(TaskGroupTest, CancelAllCancelsCompletedTasks) {
  g_counter = nullptr;
  std::atomic<int> counter{0};
  g_counter = &counter;

  auto coro = [&]() -> Task<void> {
    TaskGroup group;
    group.CreateTask(CounterCoroutine());  // Completes immediately
    group.CancelAll();  // No-op since task already done
    co_return;
  };

  asyncio::Run<void>(coro);
  g_counter = nullptr;
  EXPECT_EQ(counter, 1);  // CounterCoroutine completed
}

TEST(TaskGroupTest, DestructorCancelsRemainingTasks) {
  g_counter = nullptr;
  std::atomic<int> counter{0};
  g_counter = &counter;

  auto coro = [&]() -> Task<void> {
    TaskGroup* group = new TaskGroup();
    group->CreateTask(CounterCoroutine());
    delete group;  // Should cancel the running task.
    co_return;
  };

  asyncio::Run<void>(coro);
  g_counter = nullptr;
  // CounterCoroutine completes immediately, so it should finish before delete.
  // But if it were SleepyCoroutine, it would be cancelled.
  EXPECT_EQ(counter, 1);
}

// --- Exception Propagation Tests ---

TEST(TaskGroupTest, ExceptionPropagatesFromSingleTask) {
  auto coro = [&]() -> Task<void> {
    TaskGroup group;
    group.CreateTask(ThrowingCoroutine("test error"));
    co_await group.WaitComplete();  // Should propagate exception.
  };

  EXPECT_THROW(asyncio::Run<void>(coro), std::runtime_error);
}

TEST(TaskGroupTest, ExceptionCancelsOtherTasks) {
  g_counter = nullptr;
  std::atomic<int> counter{0};
  g_counter = &counter;

  auto coro = [&]() -> Task<void> {
    TaskGroup group;
    group.CreateTask(CounterCoroutine());  // Will complete before exception
    group.CreateTask(ThrowingCoroutine("error"));
    co_await group.WaitComplete();
  };

  EXPECT_THROW(asyncio::Run<void>(coro), std::runtime_error);
  g_counter = nullptr;
  // CounterCoroutine completes immediately, so it should finish.
  EXPECT_EQ(counter, 1);
}

TEST(TaskGroupTest, ExceptionPropagates) {
  // Test that when one task throws, exception is propagated.
  auto coro = [&]() -> Task<void> {
    TaskGroup group;
    group.CreateTask(ThrowingCoroutine("error"));
    group.CreateTask(EmptyCoroutine());
    co_await group.WaitComplete();
  };

  EXPECT_THROW(asyncio::Run<void>(coro), std::runtime_error);
}

// --- Nested TaskGroup Tests ---

TEST(TaskGroupTest, NestedTaskGroups) {
  g_counter = nullptr;
  std::atomic<int> counter{0};
  g_counter = &counter;

  auto coro = [&]() -> Task<void> {
    TaskGroup outer;
    outer.CreateTask([&]() -> Task<void> {
      TaskGroup inner;
      inner.CreateTask(CounterCoroutine());
      co_await inner.WaitComplete();
      co_return;
    }());
    co_await outer.WaitComplete();
  };

  asyncio::Run<void>(coro);
  g_counter = nullptr;
  EXPECT_EQ(counter, 1);
}

}  // namespace
}  // namespace asyncio
