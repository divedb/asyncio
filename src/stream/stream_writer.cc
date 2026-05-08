// Copyright 2025 asyncio-cpp authors. All rights reserved.
// StreamWriter implementation.

#include "asyncio/stream/stream_writer.h"

#include <algorithm>

#include "asyncio/event_loop.h"
#include "asyncio/transport/transport.h"

namespace asyncio {

StreamWriter::StreamWriter(detail::SocketTransport& transport)
    : transport_(transport) {}

StreamWriter::~StreamWriter() = default;

void StreamWriter::Write(std::span<const uint8_t> data) {
  if (closed_) return;
  transport_.Write(data);
}

void StreamWriter::Write(std::string_view data) {
  if (closed_) return;
  transport_.Write(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()),
                                data.size()));
}

void StreamWriter::Writelines(
    std::span<std::span<const uint8_t>> data) {
  if (closed_) return;
  transport_.Writelines(data);
}

Future<void> StreamWriter::Drain() {
  if (closed_) {
    Future<void> f;
    f.SetResult();
    return f;
  }
  return transport_.Drain();
}

void StreamWriter::WriteEof() {
  if (closed_) return;
  transport_.WriteEof();
}

Future<void> StreamWriter::Close() {
  closed_ = true;
  return transport_.Close();
}

bool StreamWriter::IsClosing() const { return transport_.IsClosing(); }

Future<void> StreamWriter::WaitClosed() {
  return transport_.WaitClosed();
}

std::string StreamWriter::GetExtraInfo(const std::string& name) const {
  return transport_.GetExtraInfo(name);
}

}  // namespace asyncio
