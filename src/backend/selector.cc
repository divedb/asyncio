#include "asyncio/backend/selector_epoll.h"
#include "asyncio/backend/selector_kqueue.h"
#include "asyncio/backend/selector_select.hh"

namespace asyncio {

[[nodiscard]] std::unique_ptr<Selector> DefaultSelector() {
#if defined(ASYNCIO_OS_LINUX)
  return std::make_unique<EpollSelector>();
#elif defined(ASYNCIO_OS_APPLE)
  return std::make_unique<KqueueSelector>();
#else
  return std::make_unique<SelectSelector>();
#endif
}

}  // namespace asyncio