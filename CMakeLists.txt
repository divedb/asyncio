cmake_minimum_required(VERSION 3.20)
project(asyncio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Library
add_library(asyncio
  src/handle.cc
  src/timer_handle.cc
  src/detail/timer_heap.cc
  src/detail/self_pipe.cc
  src/detail/selector_select.cc
  src/event_loop.cc
  src/sleep.cc
)

# Platform-specific Selector backend.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(asyncio PRIVATE src/detail/selector_epoll.cc)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
       CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" OR
       CMAKE_SYSTEM_NAME STREQUAL "OpenBSD" OR
       CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
  target_sources(asyncio PRIVATE src/detail/selector_kqueue.cc)
endif()

target_include_directories(asyncio PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_options(asyncio PRIVATE
  -Wall -Wextra -Wpedantic -Werror
)

# Sanitizer flags for Debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(asyncio PRIVATE -fsanitize=address,undefined)
  target_link_options(asyncio PRIVATE -fsanitize=address,undefined)
endif()

# Platform-specific libraries for self_pipe / networking
if(APPLE)
  target_link_libraries(asyncio PUBLIC "-framework CoreFoundation")
elseif(UNIX)
  target_link_libraries(asyncio PUBLIC pthread)
endif()

# Tests
option(ASYNCIO_BUILD_TESTS "Build tests" ON)

if(ASYNCIO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
