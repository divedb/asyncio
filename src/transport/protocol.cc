// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Protocol implementation.

#include "asyncio/transport/protocol.h"

#include <algorithm>

#include "asyncio/stream/stream_reader.h"
#include "asyncio/stream/stream_writer.h"
#include "asyncio/transport/transport.h"

namespace asyncio {

void StreamProtocol::SetStreams(StreamReader* reader, StreamWriter* writer) {
  reader_ = reader;
  writer_ = writer;
}

void StreamProtocol::ConnectionMade(TransportBase& transport) {
  transport_ = &transport;
}

void StreamProtocol::DataReceived(std::span<const uint8_t> data) {
  if (reader_) {
    reader_->FeedData(data);
  }
}

void StreamProtocol::ConnectionLost(std::exception_ptr ex) {
  if (reader_ && ex) {
    reader_->SetException(ex);
  } else if (reader_) {
    reader_->FeedEof();
  }
}

void StreamProtocol::PauseWriting() {
  // The transport's write buffer is full — the application should stop
  // generating more data. This is called from SocketTransport when its
  // write buffer is full.
  if (writer_) {
    // StreamWriter doesn't have an explicit pause method; the drain
    // mechanism handles backpressure. The application should stop writing
    // and await Drain().
  }
}

void StreamProtocol::ResumeWriting() {
  // The transport's write buffer has drained below the low-water mark.
  // The application can resume writing.
}

}  // namespace asyncio
