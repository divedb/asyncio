#pragma once

#if defined(_WIN32) || defined(_WIN64)
#define ASYNCIO_OS_WINDOWS 1
#elif defined(__APPLE__)
#define ASYNCIO_OS_APPLE 1
#elif defined(__linux__)
#define ASYNCIO_OS_LINUX 1
#elif defined(__unix__)
#define ASYNCIO_OS_UNIX 1
#else
#error "asyncio: unsupported platform"
#endif
