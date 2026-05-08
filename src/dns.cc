// Copyright 2025 asyncio-cpp authors. All rights reserved.
// dns.cc — Implementation of asynchronous DNS resolution.

#include "asyncio/dns.h"

#include <netdb.h>
#include <arpa/inet.h>
#include <cstring>

#include "asyncio/event_loop.h"
#include "asyncio/socket/socket_base.h"

namespace asyncio {
namespace dns {

// ============================================================================
// Helper
// ============================================================================

static EventLoop* GetLoop(EventLoop* loop) {
  if (!loop) return EventLoop::Current();
  return loop;
}

// ============================================================================
// GetAddrInfoSync — the blocking core
// ============================================================================

static int ProtocolToInt(Protocol proto) {
  return static_cast<int>(proto);
}

std::vector<AddrInfo> GetAddrInfoSync(
    const std::string& host,
    int port,
    int family,
    int type,
    Protocol protocol) {
  std::vector<AddrInfo> results;

  struct addrinfo hints {};
  hints.ai_family = family;
  hints.ai_socktype = type;
  hints.ai_protocol = ProtocolToInt(protocol);
  // Don't set AI_NUMERICHOST — we want resolution

  std::string port_str = std::to_string(port);

  struct addrinfo* res = nullptr;
  int err = ::getaddrinfo(host.empty() ? nullptr : host.c_str(),
                          port_str.c_str(),
                          &hints,
                          &res);
  if (err != 0) {
    // Return empty vector on error; caller can check results.empty()
    return results;
  }

  for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
    AddrInfo info;
    info.family = p->ai_family;
    info.type = p->ai_socktype;
    info.protocol = p->ai_protocol;
    info.port = port;

    // Get canonical name if available
    if (p->ai_canonname) {
      info.canonical_name = p->ai_canonname;
    }

    // Get address string
    if (p->ai_family == AF_INET) {
      struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(p->ai_addr);
      char buf[INET_ADDRSTRLEN];
      if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
        info.address = buf;
      }
    } else if (p->ai_family == AF_INET6) {
      struct sockaddr_in6* sin6 = reinterpret_cast<struct sockaddr_in6*>(p->ai_addr);
      char buf[INET6_ADDRSTRLEN];
      if (::inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf))) {
        info.address = buf;
      }
    }

    results.push_back(info);
  }

  ::freeaddrinfo(res);
  return results;
}

// ============================================================================
// GetAddrInfo — async wrapper
// ============================================================================

namespace detail {

struct GetAddrInfoContext {
  std::string host;
  int port;
  int family;
  int type;
  Protocol protocol;
  Future<std::vector<AddrInfo>> future;
};

}  // namespace detail

static void DoGetAddrInfo(detail::GetAddrInfoContext* ctx) {
  std::vector<AddrInfo> results = GetAddrInfoSync(
      ctx->host,
      ctx->port,
      ctx->family,
      ctx->type,
      ctx->protocol);

  if (results.empty()) {
    ctx->future.SetException(std::make_exception_ptr(
        std::runtime_error("DNS resolution failed: no results")));
  } else {
    ctx->future.SetResult(std::move(results));
  }
  delete ctx;
}

Future<std::vector<AddrInfo>> GetAddrInfo(
    const std::string& host,
    int port,
    int family,
    int type,
    Protocol protocol,
    EventLoop* loop) {
  loop = GetLoop(loop);
  if (!loop) {
    Future<std::vector<AddrInfo>> fut;
    fut.SetException(std::make_exception_ptr(
        std::runtime_error("No event loop")));
    return fut;
  }

  // For now, DNS resolution is synchronous but scheduled on the event loop
  // to maintain async semantics. This allows future thread-pool integration.
  auto* ctx = new detail::GetAddrInfoContext{
      host, port, family, type, protocol, {}};

  loop->CallSoon([ctx]() { DoGetAddrInfo(ctx); });
  return ctx->future;
}

// ============================================================================
// GetNameInfoSync — the blocking core
// ============================================================================

NameInfo GetNameInfoSync(
    const std::string& address,
    int port,
    int flags) {
  NameInfo info;
  info.port = port;

  struct sockaddr_in sin {};
  struct sockaddr_in6 sin6 {};
  struct sockaddr* sa = nullptr;
  socklen_t salen = 0;

  // Try to parse as IPv4 first
  if (inet_pton(AF_INET, address.c_str(), &sin.sin_addr) == 1) {
    sin.sin_family = AF_INET;
    sin.sin_port = htons(static_cast<uint16_t>(port));
    sa = reinterpret_cast<struct sockaddr*>(&sin);
    salen = sizeof(sin);
  }
  // Try IPv6
  else if (inet_pton(AF_INET6, address.c_str(), &sin6.sin6_addr) == 1) {
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(static_cast<uint16_t>(port));
    sa = reinterpret_cast<struct sockaddr*>(&sin6);
    salen = sizeof(sin6);
  }

  if (!sa) {
    return info;  // Invalid address, return empty hostname
  }

  char host[NI_MAXHOST] = {};
  char serv[NI_MAXSERV] = {};

  int err = ::getnameinfo(sa, salen,
                         host, sizeof(host),
                         serv, sizeof(serv),
                         flags);
  if (err == 0) {
    info.host = host;
  }

  return info;
}

// ============================================================================
// GetNameInfo — async wrapper
// ============================================================================

namespace detail {

struct GetNameInfoContext {
  std::string address;
  int port;
  int flags;
  Future<NameInfo> future;
};

}  // namespace detail

static void DoGetNameInfo(detail::GetNameInfoContext* ctx) {
  NameInfo info = GetNameInfoSync(ctx->address, ctx->port, ctx->flags);
  ctx->future.SetResult(info);
  delete ctx;
}

Future<NameInfo> GetNameInfo(
    const std::string& address,
    int port,
    int flags,
    EventLoop* loop) {
  loop = GetLoop(loop);
  if (!loop) {
    Future<NameInfo> fut;
    fut.SetException(std::make_exception_ptr(
        std::runtime_error("No event loop")));
    return fut;
  }

  auto* ctx = new detail::GetNameInfoContext{address, port, flags, {}};

  loop->CallSoon([ctx]() { DoGetNameInfo(ctx); });
  return ctx->future;
}

}  // namespace dns
}  // namespace asyncio
