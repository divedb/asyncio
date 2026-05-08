// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for EventLoopPolicy and global convenience functions.

#include <atomic>
#include <thread>

#include "asyncio/event_loop.h"
#include "asyncio/policy.h"
#include "asyncio/runner.h"
#include "asyncio/task.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

// --- Basic Policy Tests ---

TEST(PolicyTest, DefaultPolicyCreatesEventLoop) {
  DefaultEventLoopPolicy policy;
  EventLoop* loop = policy.NewEventLoop();
  ASSERT_NE(loop, nullptr);
  delete loop;
}

TEST(PolicyTest, SetAndGetEventLoop) {
  DefaultEventLoopPolicy policy;
  EventLoop loop;

  policy.SetEventLoop(&loop);
  EXPECT_EQ(&policy.GetEventLoop(), &loop);
}

TEST(PolicyTest, GetEventLoopCreatesIfNotSet) {
  DefaultEventLoopPolicy policy;
  EventLoop& loop = policy.GetEventLoop();
  EXPECT_NE(&loop, nullptr);

  // GetEventLoop again returns the same instance.
  EventLoop& loop2 = policy.GetEventLoop();
  EXPECT_EQ(&loop, &loop2);
}

TEST(PolicyTest, GetRunningLoopReturnsNullWhenNotRunning) {
  DefaultEventLoopPolicy policy;
  EXPECT_EQ(policy.GetRunningLoop(), nullptr);
}

TEST(PolicyTest, GetRunningLoopReturnsLoopWhenRunning) {
  DefaultEventLoopPolicy policy;
  EventLoop loop;

  // Manually set as running (normally done by RunForever).
  EventLoopPolicy::SetRunningLoop(&loop);
  EXPECT_EQ(policy.GetRunningLoop(), &loop);
  EventLoopPolicy::SetRunningLoop(nullptr);
}

TEST(PolicyTest, RunForeverSetsRunningLoop) {
  DefaultEventLoopPolicy policy;
  EventLoop loop;

  std::atomic<bool> running{false};
  std::atomic<bool> check_done{false};

  std::thread t([&]() {
    policy.SetEventLoop(&loop);
    running = true;
    // GetRunningLoop should return the loop while RunForever is active.
    loop.CallSoon([&]() {
      if (asyncio::GetRunningLoop() == &loop) {
        check_done = true;
      }
      loop.Stop();
    });
    loop.RunForever();
  });

  // Wait for the thread to start and set up the loop.
  while (!running) {
    std::this_thread::yield();
  }

  t.join();
  EXPECT_TRUE(check_done);
}

// --- Global Policy Tests ---

TEST(PolicyTest, GetEventLoopPolicyReturnsNonNull) {
  EventLoopPolicy& policy = GetEventLoopPolicy();
  EXPECT_NE(&policy, nullptr);
}

TEST(PolicyTest, SetEventLoopPolicyChangesGlobalPolicy) {
  auto new_policy = std::make_unique<DefaultEventLoopPolicy>();
  EventLoopPolicy* raw = new_policy.get();

  SetEventLoopPolicy(std::move(new_policy));
  EXPECT_EQ(&GetEventLoopPolicy(), raw);
}

TEST(PolicyTest, GlobalGetEventLoopCreatesLoop) {
  // Clear any existing loop first by setting a fresh policy.
  SetEventLoopPolicy(std::make_unique<DefaultEventLoopPolicy>());

  EventLoop& loop = GetEventLoop();
  EXPECT_NE(&loop, nullptr);
}

TEST(PolicyTest, GlobalSetAndGetEventLoop) {
  SetEventLoopPolicy(std::make_unique<DefaultEventLoopPolicy>());
  EventLoop loop;

  SetEventLoop(&loop);
  EXPECT_EQ(&GetEventLoop(), &loop);
}

TEST(PolicyTest, GlobalNewEventLoopCreatesNewLoop) {
  SetEventLoopPolicy(std::make_unique<DefaultEventLoopPolicy>());
  EventLoop* loop1 = NewEventLoop();
  EventLoop* loop2 = NewEventLoop();

  EXPECT_NE(loop1, loop2);
  delete loop1;
  delete loop2;
}

TEST(PolicyTest, GlobalGetRunningLoopReturnsNullWhenNotRunning) {
  SetEventLoopPolicy(std::make_unique<DefaultEventLoopPolicy>());
  EXPECT_EQ(GetRunningLoop(), nullptr);
}

// --- Per-Thread Loop Isolation Test ---

TEST(PolicyTest, DifferentThreadsGetDifferentLoops) {
  SetEventLoopPolicy(std::make_unique<DefaultEventLoopPolicy>());

  EventLoop* main_loop = &GetEventLoop();
  EventLoop* thread_loop = nullptr;

  std::thread t([&]() {
    thread_loop = &GetEventLoop();
  });
  t.join();

  // Each thread should have its own loop.
  EXPECT_NE(main_loop, thread_loop);
}

// --- Integration with Run() ---

namespace {
// Thread-local flag for communicating between test and coroutine.
inline thread_local bool running_loop_check_flag = false;

Task<int> CoroutineWithRunningLoopCheck() {
  if (asyncio::GetRunningLoop() != nullptr) {
    running_loop_check_flag = true;
  }
  co_return 42;
}
}  // namespace

TEST(PolicyTest, RunSetsRunningLoop) {
  SetEventLoopPolicy(std::make_unique<DefaultEventLoopPolicy>());
  running_loop_check_flag = false;

  int result = asyncio::Run<int>(CoroutineWithRunningLoopCheck);
  EXPECT_EQ(result, 42);
  EXPECT_TRUE(running_loop_check_flag);
}

// --- Custom Policy Test ---

class CountingPolicy : public EventLoopPolicy {
 public:
  int loops_created = 0;

  EventLoop* NewEventLoop() override {
    loops_created++;
    return new EventLoop();
  }
};

TEST(PolicyTest, CustomPolicyCanTrackLoopCreation) {
  auto policy = std::make_unique<CountingPolicy>();
  CountingPolicy* raw = policy.get();

  SetEventLoopPolicy(std::move(policy));

  EventLoop* loop1 = NewEventLoop();
  EventLoop* loop2 = NewEventLoop();

  EXPECT_EQ(raw->loops_created, 2);
  EXPECT_NE(loop1, loop2);

  delete loop1;
  delete loop2;

  // Restore default policy.
  SetEventLoopPolicy(std::make_unique<DefaultEventLoopPolicy>());
}

}  // namespace
}  // namespace asyncio
