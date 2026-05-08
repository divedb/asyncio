// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for StreamReader, StreamWriter, and connection factory.

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <optional>
#include <tuple>
#include <vector>

#include "asyncio/event_loop.h"
#include "asyncio/stream/connection.h"
#include "asyncio/stream/stream_reader.h"
#include "asyncio/stream/stream_writer.h"
#include "asyncio/transport/protocol.h"
#include "asyncio/transport/transport.h"
#include "gtest/gtest.h"

namespace asyncio {
namespace {

// ---------------------------------------------------------------------------
// Helper: create span<const uint8_t> from string literal
// ---------------------------------------------------------------------------

inline std::span<const uint8_t> MakeSpan(const char* s) {
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
}

inline std::span<const uint8_t> MakeSpan(const std::string& s) {
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// ---------------------------------------------------------------------------
// Helper: non-blocking socket pair
// ---------------------------------------------------------------------------

struct SocketPair {
  int read_fd = -1;
  int write_fd = -1;

  SocketPair() {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      std::perror("socketpair failed");
      std::abort();
    }
    read_fd = fds[0];
    write_fd = fds[1];
    for (int fd : fds) {
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        std::perror("fcntl O_NONBLOCK failed");
        std::abort();
      }
    }
  }

  ~SocketPair() {
    if (read_fd >= 0) ::close(read_fd);
    if (write_fd >= 0) ::close(write_fd);
  }

  void Write(const char* data, int n) {
    ssize_t r = ::write(write_fd, data, static_cast<size_t>(n));
    if (r != n) {
      // Partial write or error — retry or abort.
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // Would block — this shouldn't happen in tests easily.
      }
    }
  }

  // Read whatever is available (non-blocking).
  std::vector<char> ReadAvailable(int max_n = 65536) {
    std::vector<char> buf(static_cast<size_t>(max_n));
    ssize_t r = ::read(read_fd, buf.data(), static_cast<size_t>(max_n));
    if (r > 0) {
      buf.resize(static_cast<size_t>(r));
      return buf;
    }
    return {};
  }

  SocketPair(const SocketPair&) = delete;
  SocketPair& operator=(const SocketPair&) = delete;
};

// ---------------------------------------------------------------------------
// Helper: run the event loop until a condition is met or timeout.
// ---------------------------------------------------------------------------

void RunLoopUntil(EventLoop& loop,
                  std::function<bool()> condition,
                  std::chrono::milliseconds timeout =
                      std::chrono::milliseconds(2000)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!condition() && std::chrono::steady_clock::now() < deadline) {
    loop.RunOnce();
  }
}

// ---------------------------------------------------------------------------
// StreamReader tests
// ---------------------------------------------------------------------------

class StreamReaderTest : public ::testing::Test {
 protected:
  asyncio::EventLoop loop;
};

TEST_F(StreamReaderTest, ReadReturnsEmptyImmediatelyIfNoData) {
  StreamReader reader(loop);

  bool done = false;
  auto fut = reader.Read(10);
  fut.AddDoneCallback([&](Future<std::vector<uint8_t>>&) { done = true; });

  EXPECT_FALSE(done);
  loop.RunOnce();  // Should not resolve (no data).
  EXPECT_FALSE(done);

  // Feed data.
  reader.FeedData(MakeSpan("hello"));
  EXPECT_TRUE(done);
  EXPECT_EQ(fut.Result(), std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}));

  EXPECT_FALSE(reader.AtEof());
}

TEST_F(StreamReaderTest, ReadReturnsDataImmediatelyIfAvailable) {
  StreamReader reader(loop);

  reader.FeedData(MakeSpan("hello"));

  auto fut = reader.Read(10);
  EXPECT_TRUE(fut.Done());
  EXPECT_EQ(fut.Result(), std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}));
}

TEST_F(StreamReaderTest, ReadExactlyReturnsDataWhenEnoughAvailable) {
  StreamReader reader(loop);

  reader.FeedData(MakeSpan("hello world"));

  auto fut = reader.ReadExactly(5);
  EXPECT_TRUE(fut.Done());
  EXPECT_EQ(fut.Result(), std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}));

  // Extra bytes should be in buffer.
  EXPECT_TRUE(reader.IsReadable());
}

TEST_F(StreamReaderTest, ReadExactlySuspendsUntilEnoughData) {
  StreamReader reader(loop);

  auto fut = reader.ReadExactly(5);
  EXPECT_FALSE(fut.Done());

  reader.FeedData(MakeSpan("he"));
  EXPECT_FALSE(fut.Done());

  reader.FeedData(MakeSpan("llo"));
  EXPECT_TRUE(fut.Done());
  EXPECT_EQ(fut.Result(), std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}));
}

TEST_F(StreamReaderTest, ReadExactlyThrowsOnEofBeforeComplete) {
  StreamReader reader(loop);

  reader.FeedData(MakeSpan("hello"));
  reader.FeedEof();

  auto fut = reader.ReadExactly(10);
  EXPECT_TRUE(fut.Done());
  EXPECT_THROW(fut.Result(), IncompleteReadError);
}

TEST_F(StreamReaderTest, ReadUntilReturnsDataWithSeparator) {
  StreamReader reader(loop);

  reader.FeedData(MakeSpan("hello\nworld"));

  auto fut = reader.ReadUntil("\n");
  EXPECT_TRUE(fut.Done());
  std::vector<uint8_t> expected = {'h', 'e', 'l', 'l', 'o', '\n'};
  EXPECT_EQ(fut.Result(), expected);
}

TEST_F(StreamReaderTest, ReadUntilSuspendsUntilSeparator) {
  StreamReader reader(loop);

  auto fut = reader.ReadUntil("\n");
  EXPECT_FALSE(fut.Done());

  reader.FeedData(MakeSpan("hel"));
  EXPECT_FALSE(fut.Done());

  reader.FeedData(MakeSpan("lo\n"));
  EXPECT_TRUE(fut.Done());
  std::vector<uint8_t> expected = {'h', 'e', 'l', 'l', 'o', '\n'};
  EXPECT_EQ(fut.Result(), expected);
}

TEST_F(StreamReaderTest, ReadUntilThrowsOnLimitOverrun) {
  StreamReader reader(loop);
  reader.SetLimit(10);

  // Feed data without separator, exceeding limit.
  std::string long_data(15, 'x');
  reader.FeedData(MakeSpan(long_data));

  auto fut = reader.ReadUntil("\n");
  EXPECT_TRUE(fut.Done());
  EXPECT_THROW(fut.Result(), LimitOverrunError);
}

TEST_F(StreamReaderTest, FeedEofResolvesPendingReadWithEmpty) {
  StreamReader reader(loop);

  auto fut = reader.Read(10);
  EXPECT_FALSE(fut.Done());

  reader.FeedEof();
  EXPECT_TRUE(fut.Done());
  EXPECT_EQ(fut.Result(), std::vector<uint8_t>{});
  EXPECT_TRUE(reader.AtEof());
}

TEST_F(StreamReaderTest, SetExceptionResolvesPendingReadWithException) {
  StreamReader reader(loop);

  auto fut = reader.Read(10);
  EXPECT_FALSE(fut.Done());

  try {
    throw AsyncError("test error");
  } catch (...) {
    reader.SetException(std::current_exception());
  }

  EXPECT_TRUE(fut.Done());
  EXPECT_THROW(fut.Result(), AsyncError);
}

TEST_F(StreamReaderTest, MultipleReadsQueued) {
  StreamReader reader(loop);

  // Feed some data.
  reader.FeedData(MakeSpan("hello"));

  auto fut1 = reader.Read(3);
  auto fut2 = reader.Read(3);

  // First read resolves with 3 bytes.
  EXPECT_TRUE(fut1.Done());
  EXPECT_EQ(fut1.Result(), std::vector<uint8_t>({'h', 'e', 'l'}));

  // Second read should resolve with remaining 2 bytes (Python semantics: up to n).
  EXPECT_TRUE(fut2.Done());
  EXPECT_EQ(fut2.Result(), std::vector<uint8_t>({'l', 'o'}));
}

TEST_F(StreamReaderTest, AtEofIsFalseBeforeEof) {
  StreamReader reader(loop);
  EXPECT_FALSE(reader.AtEof());

  reader.FeedData(MakeSpan("hello"));
  EXPECT_FALSE(reader.AtEof());

  reader.FeedEof();
  EXPECT_TRUE(reader.AtEof());
}

TEST_F(StreamReaderTest, ReadLineIsReadUntilNewline) {
  StreamReader reader(loop);

  reader.FeedData(MakeSpan("hello\nworld\n"));

  auto fut1 = reader.ReadLine();
  EXPECT_TRUE(fut1.Done());
  EXPECT_EQ(fut1.Result(), "hello\n");

  auto fut2 = reader.ReadLine();
  EXPECT_TRUE(fut2.Done());
  EXPECT_EQ(fut2.Result(), "world\n");
}

// ---------------------------------------------------------------------------
// StreamWriter tests (using socket pair)
// ---------------------------------------------------------------------------

class StreamWriterTest : public ::testing::Test {
 protected:
  asyncio::EventLoop loop;
  SocketPair sp;
};

TEST_F(StreamWriterTest, WriteSendsDataToSocket) {
  // Create a simple protocol (null).
  class NullProtocol : public ProtocolBase {
   public:
    void ConnectionMade(TransportBase&) override {}
    void DataReceived(std::span<const uint8_t>) override {}
    void ConnectionLost(std::exception_ptr) override {}
  };
  NullProtocol protocol;

  auto transport = std::make_shared<detail::SocketTransport>(
      loop, sp.write_fd, protocol, nullptr, nullptr);
  StreamWriter writer(*transport);

  writer.Write(MakeSpan("hello"));

  // Run loop to process write.
  loop.RunOnce();

  // Read from the other end.
  auto data = sp.ReadAvailable();
  EXPECT_EQ(data, std::vector<char>({'h', 'e', 'l', 'l', 'o'}));
}

TEST_F(StreamWriterTest, DrainCompletesAfterWrite) {
  class NullProtocol : public ProtocolBase {
   public:
    void ConnectionMade(TransportBase&) override {}
    void DataReceived(std::span<const uint8_t>) override {}
    void ConnectionLost(std::exception_ptr) override {}
  };
  NullProtocol protocol;

  auto transport = std::make_shared<detail::SocketTransport>(
      loop, sp.write_fd, protocol, nullptr, nullptr);
  StreamWriter writer(*transport);

  writer.Write(MakeSpan("hello"));

  bool drain_done = false;
  auto drain_fut = writer.Drain();
  drain_fut.AddDoneCallback([&](Future<void>&) { drain_done = true; });

  RunLoopUntil(loop, [&]() { return drain_done; });
  EXPECT_TRUE(drain_done);
}

TEST_F(StreamWriterTest, WriteEofSendsHalfClose) {
  class NullProtocol : public ProtocolBase {
   public:
    void ConnectionMade(TransportBase&) override {}
    void DataReceived(std::span<const uint8_t>) override {}
    void ConnectionLost(std::exception_ptr) override {}
  };
  NullProtocol protocol;

  auto transport = std::make_shared<detail::SocketTransport>(
      loop, sp.write_fd, protocol, nullptr, nullptr);
  StreamWriter writer(*transport);

  writer.Write(MakeSpan("hello"));
  writer.WriteEof();

  // Run loop to process write.
  RunLoopUntil(loop, [&]() { return transport->IsClosing(); });

  // Read from the other end.
  auto data = sp.ReadAvailable();
  EXPECT_EQ(data, std::vector<char>({'h', 'e', 'l', 'l', 'o'}));
}

TEST_F(StreamWriterTest, IsClosingReflectsTransportState) {
  class NullProtocol : public ProtocolBase {
   public:
    void ConnectionMade(TransportBase&) override {}
    void DataReceived(std::span<const uint8_t>) override {}
    void ConnectionLost(std::exception_ptr) override {}
  };
  NullProtocol protocol;

  auto transport = std::make_shared<detail::SocketTransport>(
      loop, sp.write_fd, protocol, nullptr, nullptr);
  StreamWriter writer(*transport);

  EXPECT_FALSE(writer.IsClosing());

  writer.Close();

  EXPECT_TRUE(writer.IsClosing());
}

// ---------------------------------------------------------------------------
// SocketTransport tests (using socket pair)
// ---------------------------------------------------------------------------

class SocketTransportTest : public ::testing::Test {
 protected:
  asyncio::EventLoop loop;
};

TEST_F(SocketTransportTest, ReadFromSocketPair) {
  class NullProtocol : public ProtocolBase {
   public:
    void ConnectionMade(TransportBase&) override {}
    void DataReceived(std::span<const uint8_t>) override {}
    void ConnectionLost(std::exception_ptr) override {}
  };
  NullProtocol protocol;

  SocketPair sp;

  auto transport = std::make_shared<detail::SocketTransport>(
      loop, sp.write_fd, protocol, nullptr, nullptr);

  // Write data from the "client" side.
  sp.Write("hello", 5);

  // Run loop to process.
  loop.RunOnce();

  // Read from transport.
  auto fut = transport->Read(5);

  RunLoopUntil(loop, [&]() { return fut.Done(); });

  EXPECT_TRUE(fut.Done());
  EXPECT_EQ(fut.Result(), std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}));
}

TEST_F(SocketTransportTest, WriteToSocketPair) {
  class NullProtocol : public ProtocolBase {
   public:
    void ConnectionMade(TransportBase&) override {}
    void DataReceived(std::span<const uint8_t>) override {}
    void ConnectionLost(std::exception_ptr) override {}
  };
  NullProtocol protocol;

  SocketPair sp;

  auto transport = std::make_shared<detail::SocketTransport>(
      loop, sp.write_fd, protocol, nullptr, nullptr);

  auto fut = transport->WriteAll(MakeSpan("hello"));

  RunLoopUntil(loop, [&]() { return fut.Done(); });

  EXPECT_TRUE(fut.Done());

  auto data = sp.ReadAvailable();
  EXPECT_EQ(data, std::vector<char>({'h', 'e', 'l', 'l', 'o'}));
}

TEST_F(SocketTransportTest, StreamReaderIntegration) {
  class TestProtocol : public ProtocolBase {
   public:
    TestProtocol(StreamReader* r) : reader(r) {}
    StreamReader* reader;

    void ConnectionMade(TransportBase&) override {}
    void DataReceived(std::span<const uint8_t> data) override {
      reader->FeedData(data);
    }
    void ConnectionLost(std::exception_ptr) override { reader->FeedEof(); }
  };

  SocketPair sp;

  auto* reader = new StreamReader(loop);
  TestProtocol protocol(reader);

  auto transport = std::make_shared<detail::SocketTransport>(
      loop, sp.write_fd, protocol, reader, nullptr);
  protocol.reader = reader;  // Update after transport construction.

  // Write data.
  sp.Write("testdata", 8);

  // Run loop to process.
  RunLoopUntil(loop, [&]() { return reader->IsReadable(); });

  auto fut = reader->Read(4);
  EXPECT_TRUE(fut.Done());
  EXPECT_EQ(fut.Result(), std::vector<uint8_t>({'t', 'e', 's', 't'}));
}

// ---------------------------------------------------------------------------
// Backpressure tests
// ---------------------------------------------------------------------------

class BackpressureTest : public ::testing::Test {
 protected:
  asyncio::EventLoop loop;
};

TEST_F(BackpressureTest, StreamReaderMaybePauseReturnsTrueWhenOverLimit) {
  StreamReader reader(loop);
  reader.SetLimit(10);

  // Buffer exceeds 2× limit (20 bytes).
  std::string data(25, 'x');
  reader.FeedData(MakeSpan(data));

  EXPECT_EQ(reader.BufferSize(), 25u);
  EXPECT_TRUE(reader.MaybePause());
}

TEST_F(BackpressureTest, StreamReaderMaybePauseReturnsFalseWhenUnderLimit) {
  StreamReader reader(loop);
  reader.SetLimit(100);

  std::string data(50, 'x');
  reader.FeedData(MakeSpan(data));

  EXPECT_EQ(reader.BufferSize(), 50u);
  EXPECT_FALSE(reader.MaybePause());
}

// ---------------------------------------------------------------------------
// OpenConnection test (loopback)
// ---------------------------------------------------------------------------

TEST(ConnectionTest, OpenConnectionToLocalhost) {
  // First, start a server on a local port.
  asyncio::EventLoop loop;

  int server_port = 0;
  std::atomic<int> connections_accepted{0};

  auto server_fut = StartServer(
      [&]([[maybe_unused]] StreamReader& r, [[maybe_unused]] StreamWriter& w) {
        // Echo: read and write back.
        connections_accepted++;
        loop.Stop();
      },
      "localhost",
      0,  // Port 0 = pick any free port.
      &loop);

  // Run loop to get the server started.
  RunLoopUntil(loop, [&]() { return server_fut.Done(); });
  ASSERT_TRUE(server_fut.Done());
  auto server = std::move(server_fut.Result());
  server_port = server->Port();
  ASSERT_GT(server_port, 0);

  // Now open a connection to it.
  auto conn_fut =
      OpenConnection("localhost", server_port, &loop);

  RunLoopUntil(loop, [&]() { return conn_fut.Done(); });
  EXPECT_TRUE(conn_fut.Done());
  EXPECT_NO_THROW(([&]() {
    auto [reader, writer] = conn_fut.Result();
    ASSERT_NE(reader, nullptr);
    ASSERT_NE(writer, nullptr);
  })());

  // Clean up.
  server->Close();
}

}  // namespace
}  // namespace asyncio
