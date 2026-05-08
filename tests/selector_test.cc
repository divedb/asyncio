// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Tests for Selector backends: Register/Modify/Unregister, read/write
// readiness, timeout accuracy, and EventLoop I/O integration.

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

#include "asyncio/detail/selector.h"
#include "asyncio/detail/selector_backend.h"
#include "asyncio/detail/selector_select.h"  // Always available as fallback.
#include "asyncio/event_loop.h"
#include "gtest/gtest.h"

namespace asyncio::detail {
namespace {

// ---------------------------------------------------------------------------
// Helper: create a non-blocking socket pair.
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
    if (read_fd >= 0) close(read_fd);
    if (write_fd >= 0) close(write_fd);
  }

  // Write n bytes to write_fd.
  void Write(const char* data, int n) {
    ssize_t written = write(write_fd, data, static_cast<size_t>(n));
    if (written != n) {
      std::perror("SocketPair::Write failed");
      std::abort();
    }
  }

  // Disallow copy.
  SocketPair(const SocketPair&) = delete;
  SocketPair& operator=(const SocketPair&) = delete;
};

// ---------------------------------------------------------------------------
// DefaultSelector tests
// ---------------------------------------------------------------------------

class DefaultSelectorTest : public ::testing::Test {
 protected:
  DefaultSelector selector;
};

TEST_F(DefaultSelectorTest, SelectWithZeroTimeoutReturnsImmediately) {
  // A zero timeout should never block.
  auto start = std::chrono::steady_clock::now();
  auto events = selector.Select(std::chrono::nanoseconds::zero());
  auto elapsed = std::chrono::steady_clock::now() - start;

  // Should return well under 10 ms.
  EXPECT_LT(elapsed, std::chrono::milliseconds(10));
  EXPECT_TRUE(events.empty());
}

TEST_F(DefaultSelectorTest, SelectWithSmallTimeoutBlocks) {
  auto start = std::chrono::steady_clock::now();
  auto events = selector.Select(std::chrono::milliseconds(20));
  auto elapsed = std::chrono::steady_clock::now() - start;

  // Should have waited ~20 ms (allow generous tolerance for CI).
  EXPECT_GE(elapsed, std::chrono::milliseconds(10));
  EXPECT_TRUE(events.empty());
}

TEST_F(DefaultSelectorTest, RegisteredReadableFdIsReported) {
  SocketPair sp;
  selector.Register(sp.read_fd, kReadable);

  // Make the read end readable by writing to the write end.
  sp.Write("x", 1);

  auto events = selector.Select(std::chrono::milliseconds(100));

  ASSERT_FALSE(events.empty());
  bool found = false;
  for (const auto& ev : events) {
    if (ev.fd == sp.read_fd && ev.readable()) {
      found = true;
    }
  }
  EXPECT_TRUE(found);

  selector.Unregister(sp.read_fd);
}

TEST_F(DefaultSelectorTest, RegisteredWritableFdIsReported) {
  SocketPair sp;
  selector.Register(sp.write_fd, kWritable);

  // A fresh socket pair write end is always writable.
  auto events = selector.Select(std::chrono::nanoseconds::zero());

  bool found = false;
  for (const auto& ev : events) {
    if (ev.fd == sp.write_fd && ev.writable()) {
      found = true;
    }
  }
  EXPECT_TRUE(found);

  selector.Unregister(sp.write_fd);
}

TEST_F(DefaultSelectorTest, UnregisteredFdIsNotReported) {
  SocketPair sp;
  selector.Register(sp.read_fd, kReadable);
  selector.Unregister(sp.read_fd);  // Remove immediately.

  // Even if we write data, the fd should not appear in results.
  sp.Write("x", 1);

  auto events = selector.Select(std::chrono::nanoseconds::zero());
  for (const auto& ev : events) {
    EXPECT_NE(ev.fd, sp.read_fd);
  }
}

TEST_F(DefaultSelectorTest, ModifyChangesMonitoredEvents) {
  SocketPair sp;
  // Initially register for reads only.
  selector.Register(sp.write_fd, kReadable);

  // The write end is not readable (no data), so Select should return nothing.
  auto events = selector.Select(std::chrono::nanoseconds::zero());
  bool initially_writable = false;
  for (const auto& ev : events) {
    if (ev.fd == sp.write_fd && ev.writable()) initially_writable = true;
  }
  EXPECT_FALSE(initially_writable);

  // Modify to also include writable.
  selector.Modify(sp.write_fd, kReadable | kWritable);

  events = selector.Select(std::chrono::nanoseconds::zero());
  bool now_writable = false;
  for (const auto& ev : events) {
    if (ev.fd == sp.write_fd && ev.writable()) now_writable = true;
  }
  EXPECT_TRUE(now_writable);

  selector.Unregister(sp.write_fd);
}

TEST_F(DefaultSelectorTest, ModifyUnregisteredFdThrows) {
  EXPECT_THROW(selector.Modify(999, kReadable), std::invalid_argument);
}

TEST_F(DefaultSelectorTest, UnregisterNonExistentFdIsNoOp) {
  // Should not throw.
  EXPECT_NO_THROW(selector.Unregister(9999));
}

TEST_F(DefaultSelectorTest, MultipleRegistrations) {
  SocketPair sp1, sp2;
  selector.Register(sp1.read_fd, kReadable);
  selector.Register(sp2.read_fd, kReadable);

  sp1.Write("a", 1);
  sp2.Write("b", 1);

  auto events = selector.Select(std::chrono::milliseconds(100));

  bool found1 = false, found2 = false;
  for (const auto& ev : events) {
    if (ev.fd == sp1.read_fd && ev.readable()) found1 = true;
    if (ev.fd == sp2.read_fd && ev.readable()) found2 = true;
  }
  EXPECT_TRUE(found1);
  EXPECT_TRUE(found2);

  selector.Unregister(sp1.read_fd);
  selector.Unregister(sp2.read_fd);
}

// ---------------------------------------------------------------------------
// SelectSelector tests (portable fallback — always compiled in)
// ---------------------------------------------------------------------------

class SelectSelectorTest : public ::testing::Test {
 protected:
  SelectSelector selector;
};

TEST_F(SelectSelectorTest, ZeroTimeoutReturnsImmediately) {
  auto start = std::chrono::steady_clock::now();
  auto events = selector.Select(std::chrono::nanoseconds::zero());
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, std::chrono::milliseconds(10));
  EXPECT_TRUE(events.empty());
}

TEST_F(SelectSelectorTest, ReadableSocketReported) {
  SocketPair sp;
  selector.Register(sp.read_fd, kReadable);
  sp.Write("hello", 5);

  auto events = selector.Select(std::chrono::milliseconds(100));
  bool found = false;
  for (const auto& ev : events) {
    if (ev.fd == sp.read_fd && ev.readable()) found = true;
  }
  EXPECT_TRUE(found);
  selector.Unregister(sp.read_fd);
}

TEST_F(SelectSelectorTest, WritableSocketReported) {
  SocketPair sp;
  selector.Register(sp.write_fd, kWritable);

  auto events = selector.Select(std::chrono::nanoseconds::zero());
  bool found = false;
  for (const auto& ev : events) {
    if (ev.fd == sp.write_fd && ev.writable()) found = true;
  }
  EXPECT_TRUE(found);
  selector.Unregister(sp.write_fd);
}

TEST_F(SelectSelectorTest, ModifyWorks) {
  SocketPair sp;
  selector.Register(sp.write_fd, kReadable);
  selector.Modify(sp.write_fd, kReadable | kWritable);

  auto events = selector.Select(std::chrono::nanoseconds::zero());
  bool writable = false;
  for (const auto& ev : events) {
    if (ev.fd == sp.write_fd && ev.writable()) writable = true;
  }
  EXPECT_TRUE(writable);
  selector.Unregister(sp.write_fd);
}

TEST_F(SelectSelectorTest, UnregisterRemovesFd) {
  SocketPair sp;
  selector.Register(sp.read_fd, kReadable);
  selector.Unregister(sp.read_fd);
  sp.Write("x", 1);

  auto events = selector.Select(std::chrono::nanoseconds::zero());
  for (const auto& ev : events) {
    EXPECT_NE(ev.fd, sp.read_fd);
  }
}

TEST_F(SelectSelectorTest, ModifyUnregisteredFdThrows) {
  EXPECT_THROW(selector.Modify(999, kReadable), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// EventLoop I/O integration tests (AddReader / AddWriter)
// ---------------------------------------------------------------------------

class EventLoopIoTest : public ::testing::Test {
 protected:
  asyncio::EventLoop loop;
};

TEST_F(EventLoopIoTest, AddReaderCallbackFiredWhenDataAvailable) {
  SocketPair sp;
  bool called = false;

  loop.AddReader(sp.read_fd, [&]() {
    called = true;
    loop.Stop();
  });

  // Write data from a CallSoon callback to simulate async write.
  loop.CallSoon([&sp]() {
    sp.Write("ping", 4);
  });

  loop.RunForever();
  EXPECT_TRUE(called);

  loop.RemoveReader(sp.read_fd);
}

TEST_F(EventLoopIoTest, AddWriterCallbackFiredForWritableFd) {
  SocketPair sp;
  bool called = false;

  loop.AddWriter(sp.write_fd, [&]() {
    called = true;
    loop.Stop();
  });

  loop.RunForever();
  EXPECT_TRUE(called);

  loop.RemoveWriter(sp.write_fd);
}

TEST_F(EventLoopIoTest, RemoveReaderStopsCallbacks) {
  SocketPair sp;
  int call_count = 0;

  loop.AddReader(sp.read_fd, [&]() {
    call_count++;
    loop.RemoveReader(sp.read_fd);
    loop.Stop();
  });

  sp.Write("x", 1);
  loop.RunForever();

  // Callback should fire once, then be removed.
  EXPECT_EQ(call_count, 1);
}

TEST_F(EventLoopIoTest, RemoveWriterStopsCallbacks) {
  SocketPair sp;
  int call_count = 0;

  loop.AddWriter(sp.write_fd, [&]() {
    call_count++;
    loop.RemoveWriter(sp.write_fd);
    loop.Stop();
  });

  loop.RunForever();
  EXPECT_EQ(call_count, 1);
}

TEST_F(EventLoopIoTest, ReaderAndTimerCoexist) {
  SocketPair sp;
  bool timer_fired = false;
  bool reader_fired = false;

  loop.AddReader(sp.read_fd, [&]() {
    reader_fired = true;
    loop.RemoveReader(sp.read_fd);
    if (timer_fired) loop.Stop();
  });

  loop.CallLater(std::chrono::milliseconds(20), [&]() {
    timer_fired = true;
    sp.Write("!", 1);  // Trigger the reader.
  });

  // Safety: stop after 2 seconds to avoid hanging.
  loop.CallLater(std::chrono::seconds(2), [&]() { loop.Stop(); });

  loop.RunForever();
  EXPECT_TRUE(timer_fired);
  EXPECT_TRUE(reader_fired);
}

}  // namespace
}  // namespace asyncio::detail
