// Copyright 2025 asyncio-cpp authors. All rights reserved.
// selector_backend.h — compile-time selection of the I/O backend.
//
// Usage:
//   #include "asyncio/detail/selector_backend.h"
//   // asyncio::detail::DefaultSelector is the best backend for this platform.
//   auto selector = std::make_unique<asyncio::detail::DefaultSelector>();

#ifndef ASYNCIO_DETAIL_SELECTOR_BACKEND_H_
#define ASYNCIO_DETAIL_SELECTOR_BACKEND_H_

#if defined(__linux__)
#  include "asyncio/detail/selector_epoll.h"
#elif defined(__APPLE__) || defined(__FreeBSD__) || \
      defined(__OpenBSD__) || defined(__NetBSD__)
#  include "asyncio/detail/selector_kqueue.h"
#else
#  include "asyncio/detail/selector_select.h"
#endif

namespace asyncio::detail {

// Alias for the best available selector on this platform.
#if defined(__linux__)
using DefaultSelector = EpollSelector;
#elif defined(__APPLE__) || defined(__FreeBSD__) || \
      defined(__OpenBSD__) || defined(__NetBSD__)
using DefaultSelector = KqueueSelector;
#else
using DefaultSelector = SelectSelector;
#endif

}  // namespace asyncio::detail

#endif  // ASYNCIO_DETAIL_SELECTOR_BACKEND_H_
