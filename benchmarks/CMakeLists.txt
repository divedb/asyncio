add_executable(select_vs_epoll_benchmark_cpp
  select_vs_epoll_benchmark.cc
)

target_link_libraries(select_vs_epoll_benchmark_cpp PRIVATE asyncio)

target_compile_options(select_vs_epoll_benchmark_cpp PRIVATE
  -Wall -Wextra -Wpedantic -Werror
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(select_vs_epoll_benchmark_cpp PRIVATE -fsanitize=address,undefined)
  target_link_options(select_vs_epoll_benchmark_cpp PRIVATE -fsanitize=address,undefined)
endif()
