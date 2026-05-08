// Copyright 2025 asyncio-cpp authors. All rights reserved.
// EventLoopPolicy implementation.

#include "asyncio/policy.h"

#include <memory>
#include <thread>

#include "asyncio/event_loop.h"

namespace asyncio {

// ---------------------------------------------------------------------------
// Thread-local storage helpers.
// ---------------------------------------------------------------------------

namespace detail {

// Thread-local slot for the current thread's event loop.
// This is used by DefaultEventLoopPolicy to store per-thread loops.
inline EventLoop*& ThreadLocalEventLoop() {
  thread_local EventLoop* loop = nullptr;
  return loop;
}

// Thread-local slot for the currently running event loop.
// This is set by EventLoop::RunForever() when the loop starts running.
inline EventLoop*& ThreadLocalRunningLoop() {
  thread_local EventLoop* running = nullptr;
  return running;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// EventLoopPolicy base class.
// ---------------------------------------------------------------------------

EventLoop* EventLoopPolicy::GetRunningLoop() const {
  return detail::ThreadLocalRunningLoop();
}

void EventLoopPolicy::SetRunningLoop(EventLoop* loop) {
  detail::ThreadLocalRunningLoop() = loop;
}

EventLoop& EventLoopPolicy::GetEventLoop() {
  auto*& loop = detail::ThreadLocalEventLoop();
  if (!loop) {
    loop = NewEventLoop();
  }
  return *loop;
}

void EventLoopPolicy::SetEventLoop(EventLoop* loop) {
  detail::ThreadLocalEventLoop() = loop;
}

// ---------------------------------------------------------------------------
// DefaultEventLoopPolicy.
// ---------------------------------------------------------------------------

EventLoop*& DefaultEventLoopPolicy::ThreadLocalLoop() {
  return detail::ThreadLocalEventLoop();
}

EventLoop*& DefaultEventLoopPolicy::ThreadLocalRunningLoop() {
  return detail::ThreadLocalRunningLoop();
}

EventLoop& DefaultEventLoopPolicy::GetEventLoop() {
  auto*& loop = ThreadLocalLoop();
  if (!loop) {
    loop = NewEventLoop();
  }
  return *loop;
}

void DefaultEventLoopPolicy::SetEventLoop(EventLoop* loop) {
  ThreadLocalLoop() = loop;
}

EventLoop* DefaultEventLoopPolicy::NewEventLoop() {
  return new EventLoop();
}

EventLoop* DefaultEventLoopPolicy::GetRunningLoop() const {
  return ThreadLocalRunningLoop();
}

// ---------------------------------------------------------------------------
// Global policy management.
// ---------------------------------------------------------------------------

namespace {

std::unique_ptr<EventLoopPolicy>& GlobalPolicy() {
  static std::unique_ptr<EventLoopPolicy> policy =
      std::make_unique<DefaultEventLoopPolicy>();
  return policy;
}

}  // namespace

EventLoopPolicy& GetEventLoopPolicy() {
  return *GlobalPolicy();
}

void SetEventLoopPolicy(std::unique_ptr<EventLoopPolicy> policy) {
  GlobalPolicy() = std::move(policy);
}

EventLoop& GetEventLoop() {
  return GetEventLoopPolicy().GetEventLoop();
}

void SetEventLoop(EventLoop* loop) {
  GetEventLoopPolicy().SetEventLoop(loop);
}

EventLoop* NewEventLoop() {
  return GetEventLoopPolicy().NewEventLoop();
}

EventLoop* GetRunningLoop() {
  return GetEventLoopPolicy().GetRunningLoop();
}

}  // namespace asyncio
