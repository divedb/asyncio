// Copyright 2025 asyncio-cpp authors. All rights reserved.
// SocketTransport implementation.

#include "asyncio/transport/transport.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "asyncio/event_loop.h"
#include "asyncio/stream/stream_reader.h"
#include "asyncio/stream/stream_writer.h"
#include "asyncio/transport/protocol.h"

namespace asyncio {
namespace detail {

namespace {

// Makes a file descriptor non-blocking.
void SetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) flags = 0;
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

// ---------------------------------------------------------------------------
// SocketAddress
// ---------------------------------------------------------------------------

SocketAddress::SocketAddress() { std::memset(&addr_, 0, sizeof(addr_)); }

SocketAddress::SocketAddress(const struct sockaddr_storage& addr)
    : addr_(addr) {}

bool SocketAddress::IsValid() const {
  struct sockaddr_storage tmp = addr_;
  auto* sin = reinterpret_cast<struct sockaddr_in*>(&tmp);
  return sin->sin_family != 0;
}

std::string SocketAddress::ToString() const {
  char host[INET6_ADDRSTRLEN];
  int port = 0;

  if (addr_.ss_family == AF_INET) {
    auto* sin = reinterpret_cast<const struct sockaddr_in*>(&addr_);
    inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host));
    port = ntohs(sin->sin_port);
  } else if (addr_.ss_family == AF_INET6) {
    auto* sin6 = reinterpret_cast<const struct sockaddr_in6*>(&addr_);
    inet_ntop(AF_INET6, &sin6->sin6_addr, host, sizeof(host));
    port = ntohs(sin6->sin6_port);
  }

  return std::string(host) + ":" + std::to_string(port);
}

// ---------------------------------------------------------------------------
// SocketTransport
// ---------------------------------------------------------------------------

SocketTransport::SocketTransport(EventLoop& loop,
                                 int fd,
                                 ProtocolBase& protocol,
                                 StreamReader* reader,
                                 StreamWriter*)
    : loop_(&loop),
      fd_(fd),
      protocol_(&protocol),
      reader_(reader) {
  SetNonBlocking(fd_);

  // Register socket read callback.
  loop_->AddReader(fd_, [this]() { OnSocketReadable(); });
  // Initially only register for reading; writing starts on demand.
  loop_->AddWriter(fd_, [this]() { OnSocketWritable(); });

  // Set up reader's resume callback.
  if (reader_) {
    reader_->Pause();
    reader_->Resume();
  }
}

SocketTransport::~SocketTransport() {
  if (fd_ >= 0) {
    loop_->RemoveReader(fd_);
    loop_->RemoveWriter(fd_);
    ::close(fd_);
    fd_ = -1;
  }
}

void SocketTransport::Abort() {
  if (closing_) return;
  closing_ = true;

  if (fd_ >= 0) {
    loop_->RemoveReader(fd_);
    loop_->RemoveWriter(fd_);
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    fd_ = -1;
  }

  if (protocol_) {
    std::exception_ptr ex;
    try {
      throw AsyncError("Connection aborted");
    } catch (...) {
      ex = std::current_exception();
    }
    protocol_->ConnectionLost(ex);
  }

  // Resolve wait-closed future.
  if (wait_closed_future_ && !wait_closed_future_->Done()) {
    wait_closed_future_->SetResult();
  }
  wait_closed_future_.reset();
}

std::string SocketTransport::GetExtraInfo(const std::string& name) const {
  if (name == "socket" && fd_ >= 0) {
    return "socket:" + std::to_string(fd_);
  }

  if (fd_ >= 0 && (name == "peername" || name == "sockname")) {
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (name == "peername") {
      if (getpeername(fd_, reinterpret_cast<struct sockaddr*>(&addr), &len) ==
          0) {
        return SocketAddress(addr).ToString();
      }
    } else {
      if (getsockname(fd_, reinterpret_cast<struct sockaddr*>(&addr), &len) ==
          0) {
        return SocketAddress(addr).ToString();
      }
    }
  }

  return "";
}

void SocketTransport::PauseReading() {
  reading_paused_ = true;
  loop_->RemoveReader(fd_);
}

void SocketTransport::ResumeReading() {
  reading_paused_ = false;
  loop_->AddReader(fd_, [this]() { OnSocketReadable(); });
  // Trigger a read attempt immediately if there's buffered data pending.
  OnSocketReadable();
}

void SocketTransport::SetProtocol(ProtocolBase& protocol) {
  protocol_ = &protocol;
}

void SocketTransport::Write(std::span<const uint8_t> data) {
  if (closing_ || fd_ < 0) return;
  write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());
  PerformWrite();
}

void SocketTransport::Writelines(
    std::span<std::span<const uint8_t>> data) {
  for (const auto& chunk : data) {
    Write(chunk);
  }
}

void SocketTransport::WriteEof() {
  if (eof_written_) return;
  eof_written_ = true;
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_WR);
  }
}

Future<void> SocketTransport::Drain() {
  if (fd_ < 0 || (write_buffer_.empty() && !writing_)) {
    Future<void> f;
    f.SetResult();
    return f;
  }
  if (!drain_future_) {
    drain_future_ = std::make_shared<Future<void>>();
  }
  // Return a copy of the shared drain future.
  return *drain_future_;
}

Future<void> SocketTransport::Close() {
  if (fd_ < 0) {
    Future<void> f;
    f.SetResult();
    return f;
  }

  closing_ = true;
  WriteEof();

  // Stage 1: wait for write buffer to drain.
  close_stage_ = 1;

  // Resolve any pending write future as cancelled.
  if (write_future_ && !write_future_->Done()) {
    write_future_->Cancel();
    write_future_ = nullptr;
  }

  return close_future_;
}

Future<void> SocketTransport::WaitClosed() {
  if (fd_ < 0) {
    // Already closed.
    Future<void> f;
    f.SetResult();
    return f;
  }
  if (!wait_closed_future_) {
    // Lazily create the shared future.
    wait_closed_future_ = std::make_shared<Future<void>>();
  }
  return *wait_closed_future_;  // Copy of shared future — resolves together.
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

Future<std::vector<uint8_t>> SocketTransport::Read(size_t n) {
  Future<std::vector<uint8_t>> result;

  if (fd_ < 0 || closing_) {
    result.SetResult(std::vector<uint8_t>{});
    return result;
  }

  // Non-blocking read attempt first.
  std::vector<char> buf(n);
  ssize_t r = ::read(fd_, buf.data(), n);
  if (r > 0) {
    std::vector<uint8_t> out(buf.begin(), buf.begin() + r);
    result.SetResult(std::move(out));
    return result;
  }
  if (r == 0) {
    // Clean EOF.
    result.SetResult(std::vector<uint8_t>{});
    return result;
  }
  // r < 0
  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    // Register for read event and resolve when data arrives.
    read_future_ = &result;
    return result;
  }
  // Error.
  try {
    throw AsyncError(std::string("read error: ") + std::strerror(errno));
  } catch (...) {
    result.SetException(std::current_exception());
  }
  return result;
}


Future<void> SocketTransport::WriteAll(std::span<const uint8_t> data) {
  Future<void> result;

  if (fd_ < 0 || closing_) {
    result.SetResult();
    return result;
  }

  // Non-blocking write attempt first.
  ssize_t w = ::write(fd_, data.data(), data.size());
  if (w == static_cast<ssize_t>(data.size())) {
    // All written synchronously.
    result.SetResult();
    return result;
  }
  if (w > 0) {
    // Partial write.
    std::vector<uint8_t> remaining(data.begin() + w, data.end());
    write_buffer_ = std::move(remaining);
  } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
    try {
      throw AsyncError(std::string("write error: ") + std::strerror(errno));
    } catch (...) {
      result.SetException(std::current_exception());
    }
    return result;
  }

  write_future_ = &result;
  PerformWrite();
  return result;
}

// ---------------------------------------------------------------------------
// Internal I/O
// ---------------------------------------------------------------------------

void SocketTransport::OnSocketReadable() {
  if (reading_paused_ || fd_ < 0) return;

  // Perform one non-blocking read.
  std::vector<char> buf(65536);  // 64 KiB read buffer.
  ssize_t r = ::read(fd_, buf.data(), buf.size());

  if (r > 0) {
    if (reader_) {
      reader_->FeedData(
          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(buf.data()),
                                    static_cast<size_t>(r)));
    }
    // Check backpressure after feeding data.
    if (reader_ && reader_->MaybePause()) {
      PauseReading();
      reader_->Resume();
    }
  } else if (r == 0) {
    // EOF.
    if (reader_) reader_->FeedEof();
    if (protocol_) protocol_->ConnectionLost(nullptr);
    loop_->RemoveReader(fd_);
  } else {
    // Error.
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      if (reader_) {
        try {
          throw AsyncError(std::string("read error: ") + std::strerror(errno));
        } catch (...) {
          reader_->SetException(std::current_exception());
        }
      }
      if (protocol_) {
        try {
          throw AsyncError(std::string("read error: ") + std::strerror(errno));
        } catch (...) {
          protocol_->ConnectionLost(std::current_exception());
        }
      }
      loop_->RemoveReader(fd_);
    }
  }
}

void SocketTransport::OnSocketWritable() {
  if (fd_ < 0) return;
  PerformWrite();
}

void SocketTransport::PerformWrite() {
  if (fd_ < 0 || writing_ || write_buffer_.empty()) return;

  writing_ = true;
  ssize_t w = ::write(fd_, write_buffer_.data(), write_buffer_.size());

  if (w > 0) {
    write_buffer_.erase(write_buffer_.begin(),
                         write_buffer_.begin() + static_cast<size_t>(w));
  } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
    // Write error.
    writing_ = false;
    if (write_future_ && !write_future_->Done()) {
      try {
        throw AsyncError(std::string("write error: ") + std::strerror(errno));
      } catch (...) {
        write_future_->SetException(std::current_exception());
      }
      write_future_ = nullptr;
    }
    return;
  }

  writing_ = false;

  if (write_buffer_.empty()) {
    // All data written.
    if (write_future_ && !write_future_->Done()) {
      write_future_->SetResult();
      write_future_ = nullptr;
    }
    if (drain_future_ && !drain_future_->Done()) {
      drain_future_->SetResult();
    }
    if (protocol_) protocol_->ResumeWriting();
  }
}

// ---------------------------------------------------------------------------
// Accept factory (server-side)
// ---------------------------------------------------------------------------

std::shared_ptr<SocketTransport> SocketTransport::Accept(
    EventLoop& loop,
    int server_fd,
    ProtocolBase& protocol,
    StreamReader* reader,
    StreamWriter* writer) {
  struct sockaddr_storage addr;
  socklen_t addrlen = sizeof(addr);
  int client_fd = ::accept(
      server_fd, reinterpret_cast<struct sockaddr*>(&addr), &addrlen);
  if (client_fd < 0) {
    return nullptr;
  }

  SetNonBlocking(client_fd);

  auto transport = std::make_shared<SocketTransport>(
      loop, client_fd, protocol, reader, writer);
  return transport;
}

}  // namespace detail
}  // namespace asyncio
