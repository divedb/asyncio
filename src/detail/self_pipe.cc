// Copyright 2025 asyncio-cpp authors. All rights reserved.

#include "asyncio/detail/self_pipe.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace asyncio::detail {

SelfPipe::SelfPipe() {
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    std::perror("SelfPipe: socketpair failed");
    std::abort();
  }
  read_fd_ = fds[0];
  write_fd_ = fds[1];

  // Set both ends to non-blocking.
  for (int fd : fds) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
      std::perror("SelfPipe: fcntl O_NONBLOCK failed");
      std::abort();
    }
  }
}

SelfPipe::~SelfPipe() {
  if (read_fd_ >= 0) close(read_fd_);
  if (write_fd_ >= 0) close(write_fd_);
}

int SelfPipe::ReadFd() const { return read_fd_; }

void SelfPipe::Wakeup() {
  // Writing a single byte is atomic for Unix sockets and thread-safe.
  char byte = '\0';
  ssize_t result = write(write_fd_, &byte, 1);
  (void)result;
  // EAGAIN means the pipe is already full — which is fine, the reader
  // will drain it and wake up.
}

void SelfPipe::Drain() {
  // Read up to 256 bytes at a time. Keep reading until the buffer is empty
  // (EAGAIN) to coalesce multiple wakeups.
  char buffer[256];
  while (true) {
    ssize_t n = read(read_fd_, buffer, sizeof(buffer));
    if (n <= 0) break;  // EAGAIN or error — nothing more to read.
  }
}

}  // namespace asyncio::detail
