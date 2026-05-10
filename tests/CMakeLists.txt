include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# Collect all test sources.
file(GLOB_RECURSE ASYNCIO_TEST_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc"
)

include(GoogleTest)

foreach(test_src IN LISTS ASYNCIO_TEST_SOURCES)
  get_filename_component(test_name "${test_src}" NAME_WE)

  add_executable(${test_name} "${test_src}")

  if(test_name STREQUAL "wakeup_pipe_test")
    target_link_libraries(${test_name} PRIVATE asyncio GTest::gtest)
  else()
    target_link_libraries(${test_name} PRIVATE asyncio GTest::gtest_main)
  endif()

  target_compile_options(${test_name} PRIVATE
    -Wall -Wextra -Wpedantic -Werror
  )

  # Sanitizer flags for Debug builds
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${test_name} PRIVATE -fsanitize=address,undefined)
    target_link_options(${test_name} PRIVATE -fsanitize=address,undefined)
  endif()

  gtest_discover_tests(${test_name})
endforeach()
