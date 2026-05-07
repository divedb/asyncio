// Copyright 2025 asyncio-cpp authors. All rights reserved.

#include "asyncio/detail/timer_heap.h"

#include <algorithm>
#include <utility>

namespace asyncio::detail {

void TimerHeap::Push(TimerHandle handle) {
  heap_.push_back(std::move(handle));
  SiftUp(static_cast<int>(heap_.size()) - 1);
}

TimerHandle TimerHeap::Pop() {
  // Skip cancelled entries at the top.
  while (!heap_.empty() && heap_.front().Cancelled()) {
    if (heap_.size() == 1) {
      --cancelled_count_;
      TimerHandle result = std::move(heap_.front());
      heap_.pop_back();
      return result;
    }
    std::swap(heap_.front(), heap_.back());
    heap_.pop_back();
    --cancelled_count_;
    SiftDown(0);
  }

  if (heap_.empty()) {
    return TimerHandle();  // Should not happen — caller checks Empty().
  }

  // Move the front to result, then replace front with back and sift down.
  TimerHandle result = std::move(heap_.front());
  if (heap_.size() > 1) {
    heap_.front() = std::move(heap_.back());
    heap_.pop_back();
    SiftDown(0);
  } else {
    heap_.pop_back();
  }
  return result;
}

const TimerHandle& TimerHeap::Top() const { return heap_.front(); }

bool TimerHeap::Empty() const { return heap_.empty(); }

int TimerHeap::Size() const { return static_cast<int>(heap_.size()); }

void TimerHeap::NotifyCancelled() { ++cancelled_count_; }

void TimerHeap::MaybeRebuild() {
  // Python's threshold: > 100 entries and > 50% cancelled.
  if (heap_.size() > 100 &&
      cancelled_count_ > static_cast<int>(heap_.size() / 2)) {
    Rebuild();
  }
}

void TimerHeap::Clear() {
  heap_.clear();
  cancelled_count_ = 0;
}

void TimerHeap::Rebuild() {
  std::vector<TimerHandle> alive;
  alive.reserve(heap_.size() - cancelled_count_);
  for (auto& h : heap_) {
    if (!h.Cancelled()) {
      alive.push_back(std::move(h));
    }
  }
  heap_ = std::move(alive);
  cancelled_count_ = 0;

  // Floyd's heap construction — O(n).
  for (int i = static_cast<int>(heap_.size()) / 2 - 1; i >= 0; --i) {
    SiftDown(i);
  }
}

void TimerHeap::SiftUp(int index) {
  while (index > 0) {
    int parent = Parent(index);
    if (heap_[index] < heap_[parent]) {
      std::swap(heap_[index], heap_[parent]);
      index = parent;
    } else {
      break;
    }
  }
}

void TimerHeap::SiftDown(int index) {
  int size = static_cast<int>(heap_.size());
  while (true) {
    int smallest = index;
    int left = LeftChild(index);
    int right = RightChild(index);

    if (left < size && heap_[left] < heap_[smallest]) {
      smallest = left;
    }
    if (right < size && heap_[right] < heap_[smallest]) {
      smallest = right;
    }
    if (smallest == index) break;
    std::swap(heap_[index], heap_[smallest]);
    index = smallest;
  }
}

}  // namespace asyncio::detail
