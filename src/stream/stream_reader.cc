// Copyright 2025 asyncio-cpp authors. All rights reserved.
// StreamReader implementation.

#include "asyncio/stream/stream_reader.h"

#include <algorithm>
#include <iostream>
#include <cstring>
#include <string>

#include "asyncio/event_loop.h"

namespace asyncio {

namespace {

// Copies n bytes from the front of src into dst, removing them from src.
void ConsumeFromBuffer(std::deque<uint8_t>& src,
                       std::vector<uint8_t>& dst,
                       size_t n) {
  n = std::min(n, src.size());
  dst.insert(dst.end(), src.begin(), src.begin() + static_cast<size_t>(n));
  src.erase(src.begin(), src.begin() + static_cast<size_t>(n));
}

}  // namespace

StreamReader::StreamReader(EventLoop& loop) : loop_(&loop) {}

StreamReader::~StreamReader() = default;

void StreamReader::SetLimit(size_t limit) { limit_ = limit; }

bool StreamReader::MaybePause() const {
  // Pause if buffer exceeds 2× limit.
  return buffer_.size() > 2 * limit_;
}

void StreamReader::Pause() {
  paused_ = true;
  if (resume_callback_) {
    resume_callback_();
    resume_callback_ = nullptr;
  }
}

void StreamReader::Resume() {
  paused_ = false;
}

Future<std::vector<uint8_t>> StreamReader::Read(size_t n) {
  Future<std::vector<uint8_t>> result;

  if (exception_) {
    result.SetException(exception_);
    return result;
  }

  if (!buffer_.empty()) {
    // Data available — return up to n bytes.
    std::vector<uint8_t> out;
    if (n == 0) {
      out = std::vector<uint8_t>(buffer_.begin(), buffer_.end());
      buffer_.clear();
    } else {
      ConsumeFromBuffer(buffer_, out, n);
    }
    result.SetResult(std::move(out));
    return result;
  }

  if (eof_) {
    result.SetResult(std::vector<uint8_t>{});
    return result;
  }

  // Buffer empty and not at EOF — must wait.
  ReadRequest req;
  req.n = n;
  req.exact = false;
  pending_reads_.push_back({n, false, {}, std::move(result)});
  return pending_reads_.back().future;
}

Future<std::vector<uint8_t>> StreamReader::ReadExactly(size_t n) {
  Future<std::vector<uint8_t>> result;

  if (exception_) {
    result.SetException(exception_);
    return result;
  }

  if (n == 0) {
    result.SetResult(std::vector<uint8_t>{});
    return result;
  }

  // If we already have enough data, return it immediately.
  if (buffer_.size() >= n) {
    std::vector<uint8_t> out;
    ConsumeFromBuffer(buffer_, out, n);
    result.SetResult(std::move(out));
    return result;
  }

  // If we've hit EOF before collecting n bytes, raise.
  if (eof_) {
    auto actual = static_cast<int>(buffer_.size());
    buffer_.clear();
    try {
      throw IncompleteReadError(static_cast<int>(n), actual);
    } catch (...) {
      result.SetException(std::current_exception());
    }
    return result;
  }

  // Buffer doesn't have enough, not at EOF — wait.
  ReadRequest req;
  req.n = n;
  req.exact = true;
  pending_reads_.push_back({n, true, {}, std::move(result)});
  return pending_reads_.back().future;
}

Future<std::vector<uint8_t>> StreamReader::ReadUntil(std::string_view separator) {
  Future<std::vector<uint8_t>> result;

  if (exception_) {
    result.SetException(exception_);
    return result;
  }

  if (separator.empty()) {
    try {
      throw LimitOverrunError("separator cannot be empty");
    } catch (...) {
      result.SetException(std::current_exception());
    }
    return result;
  }

  // Search in existing buffer.
  auto pos = FindSeparator(separator);
  if (pos.has_value()) {
    // Found — return data up to and including separator.
    std::vector<uint8_t> out;
    ConsumeFromBuffer(buffer_, out, pos.value() + separator.size());
    result.SetResult(std::move(out));
    return result;
  }

  // Not found — check for limit overrun.
  if (buffer_.size() > limit_) {
    buffer_.clear();
    try {
      throw LimitOverrunError("separator not found, buffer exceeded limit");
    } catch (...) {
      result.SetException(std::current_exception());
    }
    return result;
  }

  // Not found, no overrun, not at EOF — wait.
  ReadRequest req;
  req.n = 0;
  req.exact = false;
  req.separator = separator;
  pending_reads_.push_back({0, false, separator, std::move(result)});
  return pending_reads_.back().future;
}

Future<std::string> StreamReader::ReadLine() {
  // Transform Future<vector<uint8_t>> into Future<string>.
  Future<std::string> result;
  auto bytes_fut = ReadUntil("\n");
  bytes_fut.AddDoneCallback(
      [&result](Future<std::vector<uint8_t>>& fut) {
        if (fut.Done()) {
          if (fut.Cancelled()) {
            try {
              throw AsyncCancelledError();
            } catch (...) {
              result.SetException(std::current_exception());
            }
          } else {
            try {
              auto bytes = fut.Result();
              result.SetResult(std::string(bytes.begin(), bytes.end()));
            } catch (...) {
              result.SetException(std::current_exception());
            }
          }
        }
      });
  return result;
}

void StreamReader::FeedData(std::span<const uint8_t> data) {
  if (data.empty()) return;
  buffer_.insert(buffer_.end(), data.begin(), data.end());
  ResolvePendingReads();
}

void StreamReader::FeedEof() {
  eof_ = true;
  ResolvePendingReads();
}

void StreamReader::SetException(std::exception_ptr ex) {
  exception_ = std::move(ex);
  ResolvePendingReads();
}

std::function<void(std::span<const uint8_t>)> StreamReader::DataReceivedCallback() {
  return [this](std::span<const uint8_t> data) { FeedData(data); };
}

std::optional<size_t> StreamReader::FindSeparator(
    std::string_view sep) const {
  if (buffer_.empty() || sep.empty()) return std::nullopt;

  // Build a string from the buffer for searching.
  std::string s;
  s.reserve(buffer_.size());
  for (uint8_t b : buffer_) {
    s.push_back(static_cast<char>(b));
  }

  std::string_view sv(s);
  size_t pos = sv.find(sep);
  if (pos != std::string_view::npos) {
    // Return byte index of the last char of the separator.
    return pos + sep.size() - 1;
  }
  return std::nullopt;
}

void StreamReader::ResolvePendingReads() {
  while (!pending_reads_.empty()) {
    ReadRequest& req = pending_reads_.front();

    if (exception_) {
      auto f = std::move(req.future);
      pending_reads_.pop_front();
      f.SetException(exception_);
      continue;
    }

    if (req.exact) {
      // ReadExactly: need n bytes.
      if (buffer_.size() >= req.n) {
        std::vector<uint8_t> out;
        ConsumeFromBuffer(buffer_, out, req.n);
        auto f = std::move(req.future);
        pending_reads_.pop_front();
        f.SetResult(std::move(out));
      } else if (eof_) {
        auto actual = static_cast<int>(buffer_.size());
        buffer_.clear();
        auto f = std::move(req.future);
        pending_reads_.pop_front();
        try {
          throw IncompleteReadError(static_cast<int>(req.n), actual);
        } catch (...) {
          f.SetException(std::current_exception());
        }
      } else {
        // Not enough data and not at EOF — wait.
        break;
      }
    } else if (!req.separator.empty()) {
      // ReadUntil: look for separator.
      auto pos = FindSeparator(req.separator);
      if (pos.has_value()) {
        std::vector<uint8_t> out;
        size_t end_pos = pos.value() + req.separator.size();
        ConsumeFromBuffer(buffer_, out, end_pos);
        auto f = std::move(req.future);
        pending_reads_.pop_front();
        f.SetResult(std::move(out));
      } else if (buffer_.size() > limit_) {
        buffer_.clear();
        auto f = std::move(req.future);
        pending_reads_.pop_front();
        try {
          throw LimitOverrunError("separator not found, buffer exceeded limit");
        } catch (...) {
          f.SetException(std::current_exception());
        }
      } else if (eof_) {
        std::vector<uint8_t> out;
        out = std::vector<uint8_t>(buffer_.begin(), buffer_.end());
        buffer_.clear();
        auto f = std::move(req.future);
        pending_reads_.pop_front();
        f.SetResult(std::move(out));
      } else {
        break;  // Not found, wait.
      }
      } else {
        // Simple Read(n): return min(n, buffer.size()) or all if n==0.
        if (!buffer_.empty()) {
          std::vector<uint8_t> out;
          if (req.n == 0) {
            out = std::vector<uint8_t>(buffer_.begin(), buffer_.end());
            buffer_.clear();
          } else {
            ConsumeFromBuffer(buffer_, out, req.n);
          }
          auto f = std::move(req.future);
          pending_reads_.pop_front();
          f.SetResult(std::move(out));
      } else if (eof_) {
        auto f = std::move(req.future);
        pending_reads_.pop_front();
        f.SetResult(std::vector<uint8_t>{});
      } else {
        break;  // No data, not at EOF — wait.
      }
    }
  }
}

}  // namespace asyncio
