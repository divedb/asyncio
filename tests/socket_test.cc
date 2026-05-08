// Copyright 2025 asyncio-cpp authors. All rights reserved.
// socket_test.cc — Tests for async socket operations.

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "asyncio/event_loop.h"
#include "asyncio/runner.h"
#include "asyncio/socket/socket_base.h"
#include "asyncio/socket/socket_ops.h"
#include "asyncio/task.h"

using namespace asyncio;

namespace {

constexpr int kTestPort = 18080;

}  // namespace

// ============================================================================
// TcpSocket basic tests
// ============================================================================

TEST(TcpSocket, CreateAndClose) {
  auto sock = TcpSocket::Create();
  EXPECT_TRUE(sock->IsValid());
  EXPECT_TRUE(sock->IsConnected() == false);
  EXPECT_EQ(sock->Family(), AddressFamily::kIPv4);

  sock->Close();
  EXPECT_FALSE(sock->IsValid());
}

TEST(TcpSocket, ReuseAddress) {
  auto sock1 = TcpSocket::Create();
  sock1->ReuseAddress(true);

  auto sock2 = TcpSocket::Create();
  sock2->ReuseAddress(true);

  sock1->Close();
  sock2->Close();
}

TEST(TcpSocket, NonBlocking) {
  auto sock = TcpSocket::Create();
  sock->SetNonBlocking(true);
  EXPECT_TRUE(sock->IsNonBlocking());
  sock->Close();
}

// ============================================================================
// TcpListener tests
// ============================================================================

TEST(TcpListener, CreateAndListen) {
  auto listener = TcpListener::Create();
  EXPECT_TRUE(listener->IsValid());

  auto addr = SockAddr::CreateIPv4("127.0.0.1", kTestPort);
  EXPECT_EQ(::bind(listener->Fd(), addr.CAddr(), addr.Len()), 0);

  listener->Listen(128);
  EXPECT_GT(listener->Port(), 0);

  listener->Close();
}

TEST(TcpListener, LocalAddress) {
  auto listener = TcpListener::Create();
  EXPECT_TRUE(listener->IsValid());

  auto addr = SockAddr::CreateIPv4("127.0.0.1", kTestPort);
  EXPECT_EQ(::bind(listener->Fd(), addr.CAddr(), addr.Len()), 0);
  listener->Listen(128);

  auto local = listener->LocalAddress();
  EXPECT_EQ(local.Family(), AddressFamily::kIPv4);
  EXPECT_EQ(local.Port(), kTestPort);

  listener->Close();
}

// ============================================================================
// Async accept test
// ============================================================================

TEST(TcpSocketAsync, Accept) {
  std::atomic<bool> server_ready{false};
  std::atomic<bool> client_connected{false};

  auto coro = [&]() -> Task<void> {
    auto listener = TcpListener::Create();
    EXPECT_TRUE(listener->IsValid());

    listener->ReuseAddress(true);
    co_await listener->Bind("127.0.0.1", kTestPort);
    listener->Listen(5);
    server_ready = true;

    auto [client, addr] = co_await listener->Accept();
    EXPECT_TRUE(client->IsValid());
    EXPECT_EQ(addr.Family(), AddressFamily::kIPv4);
    client->Close();
    listener->Close();
  };

  auto client_coro = [&]() -> Task<void> {
    // Wait for server
    while (!server_ready) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto sock = TcpSocket::Create();
    EXPECT_TRUE(sock->IsValid());

    co_await sock->Connect("127.0.0.1", kTestPort);
    client_connected = true;
    sock->Close();
  };

  auto main_coro = [&]() -> Task<void> {
    co_await coro();
  };

  auto main_client_coro = [&]() -> Task<void> {
    co_await client_coro();
  };

  asyncio::Run<void>(main_coro);
  asyncio::Run<void>(main_client_coro);

  EXPECT_TRUE(server_ready);
  EXPECT_TRUE(client_connected);
}

// ============================================================================
// Async connect test
// ============================================================================

TEST(TcpSocketAsync, Connect) {
  // First create a listener
  auto listener = TcpListener::Create();
  listener->ReuseAddress(true);
  listener->Bind("127.0.0.1", kTestPort);
  listener->Listen(5);

  auto connect_coro = [&]() -> Task<void> {
    auto sock = TcpSocket::Create();
    EXPECT_TRUE(sock->IsValid());

    co_await sock->Connect("127.0.0.1", kTestPort);
    EXPECT_TRUE(sock->IsConnected());
    sock->Close();
  };

  asyncio::Run<void>(connect_coro);
  listener->Close();
}

// ============================================================================
// Async send/recv test
// ============================================================================

TEST(TcpSocketAsync, SendAndRecv) {
  std::string_view test_data = "Hello, Socket!";
  std::atomic<bool> server_done{false};

  auto server_coro = [&]() -> Task<void> {
    auto listener = TcpListener::Create();
    listener->ReuseAddress(true);
    co_await listener->Bind("127.0.0.1", kTestPort);
    listener->Listen(5);

    auto [client, addr] = co_await listener->Accept();

    // Receive data
    auto data = co_await client->Recv(1024);
    EXPECT_EQ(data.size(), test_data.size());
    std::string received(data.begin(), data.end());
    EXPECT_EQ(received, test_data);

    // Echo back
    co_await client->SendAll(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(test_data.data()), test_data.size()));

    client->Close();
    listener->Close();
    server_done = true;
  };

  auto client_coro = [&]() -> Task<void> {
    // Wait for listener
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto sock = TcpSocket::Create();
    co_await sock->Connect("127.0.0.1", kTestPort);

    // Send data
    co_await sock->SendAll(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(test_data.data()), test_data.size()));

    // Receive echo
    auto echo = co_await sock->Recv(1024);
    EXPECT_EQ(echo.size(), test_data.size());

    sock->Close();
  };

  asyncio::Run<void>(server_coro);
  asyncio::Run<void>(client_coro);
}

// ============================================================================
// SockAddr tests
// ============================================================================

TEST(SockAddr, CreateIPv4) {
  auto addr = SockAddr::CreateIPv4("127.0.0.1", 8080);
  EXPECT_EQ(addr.Family(), AddressFamily::kIPv4);
  EXPECT_EQ(addr.Port(), 8080);
  EXPECT_EQ(addr.Address(), "127.0.0.1");
}

TEST(SockAddr, CreateAny) {
  auto addr = SockAddr::CreateIPv4("", 0);
  EXPECT_EQ(addr.Family(), AddressFamily::kIPv4);
  EXPECT_EQ(addr.Port(), 0);
}

TEST(SockAddr, CreateIPv6) {
  auto addr = SockAddr::CreateIPv6("::1", 8080);
  EXPECT_EQ(addr.Family(), AddressFamily::kIPv6);
  EXPECT_EQ(addr.Port(), 8080);
}

// ============================================================================
// UdpSocket tests
// ============================================================================

TEST(UdpSocket, Create) {
  auto sock = UdpSocket::Create();
  EXPECT_TRUE(sock->IsValid());
  EXPECT_EQ(sock->Type(), SocketType::kDatagram);
  sock->Close();
}

TEST(UdpSocket, Bind) {
  auto bind_coro = [&]() -> Task<void> {
    auto sock = UdpSocket::Create();
    EXPECT_TRUE(sock->IsValid());

    sock->ReuseAddress(true);
    co_await sock->Bind("127.0.0.1", kTestPort);

    auto local = sock->LocalAddress();
    EXPECT_EQ(local.Port(), kTestPort);

    sock->Close();
  };

  asyncio::Run<void>(bind_coro);
}

TEST(UdpSocketAsync, SendAndRecv) {
  std::string_view test_data = "Hello, UDP!";

  auto receiver_coro = [&]() -> Task<void> {
    auto sock = UdpSocket::Create();
    sock->ReuseAddress(true);
    co_await sock->Bind("127.0.0.1", kTestPort);

    auto [data, addr] = co_await sock->RecvFrom(1024);
    EXPECT_EQ(data.size(), test_data.size());
    std::string received(data.begin(), data.end());
    EXPECT_EQ(received, test_data);
    EXPECT_EQ(addr.Family(), AddressFamily::kIPv4);

    sock->Close();
  };

  auto sender_coro = [&]() -> Task<void> {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto sock = UdpSocket::Create();
    auto addr = SockAddr::CreateIPv4("127.0.0.1", kTestPort);
    auto sent = co_await sock->SendTo(
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(test_data.data()), test_data.size()),
        addr);
    EXPECT_EQ(sent, test_data.size());
    sock->Close();
  };

  asyncio::Run<void>(receiver_coro);
  asyncio::Run<void>(sender_coro);
}

// ============================================================================
// Error handling tests
// ============================================================================

TEST(TcpSocket, ConnectRefused) {
  bool exception_caught = false;

  auto connect_coro = [&]() -> Task<void> {
    auto sock = TcpSocket::Create();
    try {
      co_await sock->Connect("127.0.0.1", kTestPort);
    } catch (const SocketException& e) {
      exception_caught = true;
    }
  };

  asyncio::Run<void>(connect_coro);
  EXPECT_TRUE(exception_caught);
}

// ============================================================================
// Multiple connections test
// ============================================================================

TEST(TcpSocketAsync, MultipleConnections) {
  constexpr int kNumClients = 5;
  std::atomic<int> client_count{0};

  auto server_coro = [&]() -> Task<void> {
    auto listener = TcpListener::Create();
    listener->ReuseAddress(true);
    co_await listener->Bind("127.0.0.1", kTestPort);
    listener->Listen(kNumClients);

    for (int i = 0; i < kNumClients; ++i) {
      auto [client, addr] = co_await listener->Accept();
      auto data = co_await client->Recv(1024);
      co_await client->SendAll(data);
      client->Close();
    }

    listener->Close();
  };

  auto client_coro = [&]() -> Task<void> {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (int i = 0; i < kNumClients; ++i) {
      auto sock = TcpSocket::Create();
      co_await sock->Connect("127.0.0.1", kTestPort);

      uint8_t data[] = {'C', 'L', 'I', static_cast<uint8_t>('0' + i)};
      co_await sock->SendAll(data);

      auto echo = co_await sock->Recv(1024);
      EXPECT_EQ(echo.size(), 4u);
      client_count++;
      sock->Close();
    }
  };

  asyncio::Run<void>(server_coro);
  asyncio::Run<void>(client_coro);

  EXPECT_EQ(client_count, kNumClients);
}
