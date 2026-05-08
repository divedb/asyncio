// Copyright 2025 asyncio-cpp authors. All rights reserved.
// dns.h — Asynchronous DNS resolution.

#ifndef ASYNCIO_DNS_H_
#define ASYNCIO_DNS_H_

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "asyncio/future.h"
#include "asyncio/socket/socket_base.h"

namespace asyncio {

class EventLoop;

namespace dns {

/// Result entry from GetAddrInfo.
///
/// Matches Python's addrinfo structure:
/// (family, type, proto, canonname, sockaddr)
///
/// In C++ we return a structured type instead of a tuple.
struct AddrInfo {
  int family;   // AF_INET or AF_INET6
  int type;     // SOCK_STREAM or SOCK_DGRAM
  int protocol; // IPPROTO_IP, IPPROTO_TCP, IPPROTO_UDP, etc.
  std::string canonical_name;  // Empty if not provided
  std::string address;         // IP string (e.g. "127.0.0.1")
  int port;                   // Port number

  AddrInfo() : family(0), type(0), protocol(0), port(0) {}
};

/// Protocol filters for GetAddrInfo.
enum class Protocol {
  kAny = 0,
  kIP = 0,     // IP protocol (generic)
  kTCP = 6,    // TCP protocol
  kUDP = 17,   // UDP protocol
};

/// Result from GetNameInfo.
struct NameInfo {
  std::string host;  // Hostname
  int port;          // Port number

  NameInfo() : port(0) {}
};

/// Asynchronously resolve host:port into addresses.
///
/// This is the low-level counterpart to TcpSocket::Connect(host, port).
/// The current implementation is synchronous (runs in the calling thread).
/// A future version may run resolution in a thread pool.
///
/// Parameters:
///   host:    Hostname or IP address string
///   port:    Port number (0 for automatic port assignment)
///   family:  Address family hint (AF_UNSPEC=0, AF_INET=2, AF_INET6=30)
///   type:    Socket type hint (SOCK_STREAM=1, SOCK_DGRAM=2, or 0 for any)
///   protocol: Protocol hint (6=TCP, 17=UDP, or 0 for any)
///   loop:    Event loop (uses Current() if nullptr)
///
/// Returns a Future that resolves to a vector of AddrInfo entries.
Future<std::vector<AddrInfo>> GetAddrInfo(
    const std::string& host,
    int port,
    int family = AF_UNSPEC,
    int type = 0,
    Protocol protocol = Protocol::kAny,
    EventLoop* loop = nullptr);

/// Perform reverse DNS lookup on a sockaddr.
///
/// Parameters:
///   address: IP address string (e.g. "93.184.216.34")
///   port:    Port number
///   flags:   NI_* flags (see getnameinfo)
///   loop:    Event loop (uses Current() if nullptr)
///
/// Returns a Future that resolves to NameInfo {hostname, port}.
Future<NameInfo> GetNameInfo(
    const std::string& address,
    int port,
    int flags = 0,
    EventLoop* loop = nullptr);

/// Synchronous versions for non-async contexts.
std::vector<AddrInfo> GetAddrInfoSync(
    const std::string& host,
    int port,
    int family = AF_UNSPEC,
    int type = 0,
    Protocol protocol = Protocol::kAny);

NameInfo GetNameInfoSync(
    const std::string& address,
    int port,
    int flags = 0);

}  // namespace dns
}  // namespace asyncio

#endif  // ASYNCIO_DNS_H_
