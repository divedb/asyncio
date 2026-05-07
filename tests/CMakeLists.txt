include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# Collect all test sources
file(GLOB_RECURSE ASYNCIO_TEST_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc"
)

add_executable(asyncio_tests ${ASYNCIO_TEST_SOURCES})

target_link_libraries(asyncio_tests PRIVATE
  asyncio
  GTest::gtest_main
)

target_compile_options(asyncio_tests PRIVATE
  -Wall -Wextra -Wpedantic -Werror
)

# Sanitizer flags for Debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(asyncio_tests PRIVATE -fsanitize=address,undefined)
  target_link_options(asyncio_tests PRIVATE -fsanitize=address,undefined)
endif()

include(GoogleTest)
gtest_discover_tests(asyncio_tests)
