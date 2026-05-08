// Copyright 2025 asyncio-cpp authors. All rights reserved.
// dns_test.cc — Tests for DNS resolution.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "asyncio/dns.h"
#include "asyncio/runner.h"

namespace {

using namespace asyncio;

TEST(DnsTest, GetAddrInfoSyncLocalhost) {
  auto results = dns::GetAddrInfoSync("localhost", 80);
  EXPECT_FALSE(results.empty());

  // Should have at least one IPv4 result
  bool has_ipv4 = false;
  for (const auto& info : results) {
    if (info.family == AF_INET) {
      has_ipv4 = true;
      EXPECT_EQ(info.port, 80);
      EXPECT_FALSE(info.address.empty());
      // Default type depends on the system's resolution; just verify it's valid
      EXPECT_TRUE(info.type == SOCK_STREAM || info.type == SOCK_DGRAM);
    }
  }
  EXPECT_TRUE(has_ipv4);
}

TEST(DnsTest, GetAddrInfoSyncIP) {
  // Resolve an IP address directly (no DNS lookup needed)
  auto results = dns::GetAddrInfoSync("127.0.0.1", 8080);
  EXPECT_FALSE(results.empty());

  bool has_ipv4 = false;
  for (const auto& info : results) {
    if (info.family == AF_INET) {
      has_ipv4 = true;
      EXPECT_EQ(info.address, "127.0.0.1");
      EXPECT_EQ(info.port, 8080);
    }
  }
  EXPECT_TRUE(has_ipv4);
}

TEST(DnsTest, GetAddrInfoSyncWithType) {
  // TCP socket
  auto tcp_results = dns::GetAddrInfoSync("localhost", 80, AF_UNSPEC, SOCK_STREAM);
  EXPECT_FALSE(tcp_results.empty());
  for (const auto& info : tcp_results) {
    EXPECT_EQ(info.type, SOCK_STREAM);
  }

  // UDP socket
  auto udp_results = dns::GetAddrInfoSync("localhost", 80, AF_UNSPEC, SOCK_DGRAM);
  EXPECT_FALSE(udp_results.empty());
  for (const auto& info : udp_results) {
    EXPECT_EQ(info.type, SOCK_DGRAM);
  }
}

TEST(DnsTest, GetAddrInfoSyncIPv6) {
  // Try to resolve localhost as IPv6
  auto results = dns::GetAddrInfoSync("localhost", 80, AF_INET6);
  // May be empty if IPv6 is not configured on this system
  // Just verify the structure is valid
  for (const auto& info : results) {
    EXPECT_EQ(info.family, AF_INET6);
  }
}

TEST(DnsTest, GetNameInfoSync) {
  auto info = dns::GetNameInfoSync("127.0.0.1", 80);
  EXPECT_EQ(info.port, 80);
  // 127.0.0.1 should resolve to "localhost"
  EXPECT_EQ(info.host, "localhost");
}

TEST(DnsTest, GetNameInfoSyncIPv6) {
  auto info = dns::GetNameInfoSync("::1", 80);
  EXPECT_EQ(info.port, 80);
  // ::1 should resolve to "localhost"
  EXPECT_EQ(info.host, "localhost");
}

// ============================================================================
// Async tests
// ============================================================================

Task<void> BasicDnsResolver() {
  auto results = co_await dns::GetAddrInfo("localhost", 80);
  EXPECT_FALSE(results.empty());

  for (const auto& info : results) {
    EXPECT_EQ(info.port, 80);
    EXPECT_FALSE(info.address.empty());
  }
}

Task<void> BasicNameResolver() {
  auto info = co_await dns::GetNameInfo("127.0.0.1", 80);
  EXPECT_EQ(info.port, 80);
}

TEST(DnsTest, DISABLED_AsyncGetAddrInfo) {
  asyncio::Run(BasicDnsResolver);
}

TEST(DnsTest, DISABLED_AsyncGetNameInfo) {
  asyncio::Run(BasicNameResolver);
}

}  // namespace
