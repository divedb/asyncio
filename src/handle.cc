// Copyright 2025 asyncio-cpp authors. All rights reserved.

#include "asyncio/handle.h"

namespace asyncio {

Handle::Handle(std::function<void()> callback)
    : state_(std::make_shared<State>()) {
  state_->callback = std::move(callback);
  state_->cancelled.store(false, std::memory_order_relaxed);
}

void Handle::Run() {
  if (!state_) return;
  if (state_->cancelled.load(std::memory_order_acquire)) return;

  // Move the callback out before invoking. This allows the callback to
  // safely re-schedule itself on the event loop.
  auto cb = std::move(state_->callback);
  state_->callback = nullptr;
  if (cb) cb();
}

bool Handle::Cancel() {
  if (!state_) return false;
  bool expected = false;
  if (state_->cancelled.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
    state_->callback = nullptr;
    return true;
  }
  return false;
}

bool Handle::Cancelled() const {
  if (!state_) return false;
  return state_->cancelled.load(std::memory_order_acquire);
}

bool Handle::Valid() const { return state_ != nullptr; }

}  // namespace asyncio
