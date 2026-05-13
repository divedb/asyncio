cmake_minimum_required(VERSION 3.20)
project(asyncio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(src)
add_subdirectory(benchmarks)
option(ASYNCIO_BUILD_TESTS "Build tests" ON)

if(ASYNCIO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
