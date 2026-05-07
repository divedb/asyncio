// Copyright 2025 asyncio-cpp authors. All rights reserved.

#include "asyncio/timer_handle.h"

namespace asyncio {

TimerHandle::TimerHandle(std::chrono::steady_clock::time_point when,
                         std::function<void()> callback)
    : Handle(std::move(callback)),
      timer_state_(std::make_shared<TimerState>(TimerState{when})) {}

std::chrono::steady_clock::time_point TimerHandle::When() const {
  return timer_state_->when;
}

bool TimerHandle::operator<(const TimerHandle& other) const {
  return timer_state_->when < other.timer_state_->when;
}

bool TimerHandle::operator>(const TimerHandle& other) const {
  return timer_state_->when > other.timer_state_->when;
}

}  // namespace asyncio
