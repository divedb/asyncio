#include "asyncio/backend/selector_select.hh"

#include <array>
#include <chrono>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace asyncio {
namespace {

TEST(SelectSelectorTest, BackendNameAndCapabilities) {
  SelectSelector selector;

  EXPECT_STREQ(selector.BackendName(), "select");

  const SelectorCapabilities caps = selector.Capabilities();
  EXPECT_TRUE(caps.level_triggered);
  EXPECT_TRUE(caps.wakeup);
  EXPECT_FALSE(caps.edge_triggered);
  EXPECT_FALSE(caps.one_shot);
  EXPECT_FALSE(caps.proactive);
}

TEST(SelectSelectorTest, RegisterInvalidHandleThrows) {
  SelectSelector selector;

  EXPECT_THROW(selector.Register(kInvalidHandle, IoEventFlags::kReadable, nullptr),
               std::invalid_argument);
}

TEST(SelectSelectorTest, ModifyUserDataAndUnregisterOnMissingHandleThrowOrNoOp) {
  SelectSelector selector;

  EXPECT_THROW(selector.Modify(kInvalidHandle, IoEventFlags::kReadable), std::invalid_argument);
  EXPECT_THROW(selector.ModifyUserData(kInvalidHandle, nullptr), std::invalid_argument);
  EXPECT_NO_THROW(selector.Unregister(kInvalidHandle));
}

TEST(SelectSelectorTest, SelectZeroTimeoutWithoutRegistrationsReturnsImmediately) {
  SelectSelector selector;

  std::array<IoEvent, 8> out{};
  const auto start = std::chrono::steady_clock::now();
  const int n = selector.Select(out, std::chrono::nanoseconds::zero());
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(n, 0);
  EXPECT_LT(elapsed, std::chrono::milliseconds(20));
}

TEST(SelectSelectorTest, NegativeTimeoutThrows) {
  SelectSelector selector;
  std::array<IoEvent, 1> out{};

  EXPECT_THROW(selector.Select(out, std::chrono::milliseconds(-1)), std::invalid_argument);
}

TEST(SelectSelectorTest, InterruptWakesBlockingSelect) {
  SelectSelector selector;
  std::array<IoEvent, 8> out{};

  std::thread waker([&selector]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    selector.Interrupt();
  });

  const auto start = std::chrono::steady_clock::now();
  const int n = selector.Select(out, std::chrono::seconds(2));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  waker.join();

  EXPECT_EQ(n, 0);
  EXPECT_LT(elapsed, std::chrono::milliseconds(300));
}

TEST(SelectSelectorTest, MultipleConcurrentInterruptsDoNotCrash) {
  SelectSelector selector;
  std::array<IoEvent, 8> out{};

  constexpr int kThreadCount = 8;
  constexpr int kInterruptsPerThread = 200;

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  // Start a blocking wait first so concurrent interrupts can race with select.
  std::thread waiter([&selector, &out]() {
    const int n = selector.Select(out, std::chrono::seconds(2));
    EXPECT_EQ(n, 0);
  });

  for (int t = 0; t < kThreadCount; ++t) {
    threads.emplace_back([&selector]() {
      for (int i = 0; i < kInterruptsPerThread; ++i) {
        selector.Interrupt();
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  waiter.join();

  // After interrupt storms, selector should remain usable and return quickly.
  const auto start = std::chrono::steady_clock::now();
  const int n = selector.Select(out, std::chrono::milliseconds(10));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(n, 0);
  EXPECT_LT(elapsed, std::chrono::milliseconds(200));
}

}  // namespace
}  // namespace asyncio
