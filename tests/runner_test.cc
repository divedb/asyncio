// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for Run() and Runner.

#include "asyncio/runner.h"
#include "asyncio/sleep.h"

#include <gtest/gtest.h>
#include <string>

// Helper coroutines — defined at global scope so TEST macros can find them
// without namespace issues.

asyncio::Task<int> ReturnValue() { co_return 42; }

asyncio::Task<std::string> SleepThenReturn() {
  co_await asyncio::Sleep(std::chrono::milliseconds(10));
  co_return "hello";
}

asyncio::Task<int> ThrowingCoroutine() {
  throw std::runtime_error("test error");
  co_return 0;
}

// Tests for free function Run()

TEST(RunnerTest, RunSynchronousCoroutine) {
  EXPECT_EQ(::asyncio::Run(ReturnValue), 42);
}

TEST(RunnerTest, RunAsynchronousCoroutine) {
  auto result = ::asyncio::Run(SleepThenReturn);
  EXPECT_EQ(result, "hello");
}

TEST(RunnerTest, RunPropagatesException) {
  EXPECT_THROW(::asyncio::Run(ThrowingCoroutine), std::runtime_error);
}

// Tests for Runner class — inside asyncio namespace to see asyncio::Runner

namespace asyncio {

TEST(RunnerTest, RunnerMultipleRuns) {
  Runner runner;
  EXPECT_EQ(runner.Run(ReturnValue), 42);
  EXPECT_EQ(runner.Run(ReturnValue), 42);
  auto result = runner.Run(SleepThenReturn);
  EXPECT_EQ(result, "hello");
}

TEST(RunnerTest, RunnerGetLoop) {
  Runner runner;
  EXPECT_NE(runner.GetLoop(), nullptr);
  runner.Close();
  EXPECT_EQ(runner.GetLoop(), nullptr);
}

TEST(RunnerTest, RunnerClose) {
  Runner runner;
  runner.Run(ReturnValue);
  runner.Close();
  // Should not crash when runner goes out of scope.
}

}  // namespace asyncio
