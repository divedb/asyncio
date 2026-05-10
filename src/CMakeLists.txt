add_library(asyncio
  backend/wakeup_pipe.cc
)

# if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
#   target_sources(asyncio PRIVATE src/detail/selector_epoll.cc)
# elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
#        CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" OR
#        CMAKE_SYSTEM_NAME STREQUAL "OpenBSD" OR
#        CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
#   target_sources(asyncio PRIVATE src/detail/selector_kqueue.cc)
# endif()

target_include_directories(asyncio PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
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
elseif(WIN32)
  target_link_libraries(asyncio PUBLIC ws2_32 mswsock)
elseif(UNIX)
  target_link_libraries(asyncio PUBLIC pthread)
endif()