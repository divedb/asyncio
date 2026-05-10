// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Subprocess implementation.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "asyncio/event_loop.h"
#include "asyncio/subprocess.h"

namespace asyncio {
namespace detail {

namespace {

int SetCloexec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int SetNonblock(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Create a pipe with both ends set to non-blocking and cloexec.
std::pair<int, int> CreatePipe() {
  int fds[2];
  if (pipe(fds) < 0) return {-1, -1};
  if (SetNonblock(fds[0]) < 0 || SetNonblock(fds[1]) < 0 ||
      SetCloexec(fds[0]) < 0 || SetCloexec(fds[1]) < 0) {
    close(fds[0]);
    close(fds[1]);
    return {-1, -1};
  }
  return {fds[0], fds[1]};
}

}  // namespace

// ---------------------------------------------------------------------------
// SubprocessTransport
// ---------------------------------------------------------------------------

SubprocessTransport::SubprocessTransport(EventLoop& loop,
                                         std::weak_ptr<SubprocessProtocol> protocol,
                                         int pid,
                                         int stdin_fd,
                                         int stdout_fd,
                                         int stderr_fd,
                                         int sync_stdin_fd)
    : loop_(loop),
      protocol_(protocol),
      pid_(pid),
      stdin_fd_(stdin_fd),
      stdout_fd_(stdout_fd),
      stderr_fd_(stderr_fd),
      sync_stdin_fd_(sync_stdin_fd) {}

SubprocessTransport::~SubprocessTransport() {
  CloseFd(stdin_fd_);
  CloseFd(stdout_fd_);
  CloseFd(stderr_fd_);
  CloseFd(sync_stdin_fd_);
}

void SubprocessTransport::CloseFd(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

void SubprocessTransport::StartReading() {
  if (stdout_fd_ >= 0) {
    loop_.AddReader(stdout_fd_, [this]() {
      std::array<uint8_t, 65536> buf{};
      ssize_t n = read(stdout_fd_, buf.data(), buf.size());
      if (n > 0) {
        OnStdoutData(std::span<const uint8_t>(buf.data(), n));
      } else if (n == 0) {
        loop_.RemoveReader(stdout_fd_);
        CloseFd(stdout_fd_);
        stdout_fd_ = -1;
        OnStdoutEof();
      } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        loop_.RemoveReader(stdout_fd_);
        CloseFd(stdout_fd_);
        stdout_fd_ = -1;
        OnStdoutEof();
      }
    });
  }

  if (stderr_fd_ >= 0) {
    loop_.AddReader(stderr_fd_, [this]() {
      std::array<uint8_t, 65536> buf{};
      ssize_t n = read(stderr_fd_, buf.data(), buf.size());
      if (n > 0) {
        OnStderrData(std::span<const uint8_t>(buf.data(), n));
      } else if (n == 0) {
        loop_.RemoveReader(stderr_fd_);
        CloseFd(stderr_fd_);
        stderr_fd_ = -1;
        OnStderrEof();
      } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        loop_.RemoveReader(stderr_fd_);
        CloseFd(stderr_fd_);
        stderr_fd_ = -1;
        OnStderrEof();
      }
    });
  }

  // Monitor child exit via SIGCHLD.
  loop_.CallSoon([this]() { OnChildExited(); });
}

void SubprocessTransport::OnStdoutData(std::span<const uint8_t> /*data*/) {
  // Data is handled by the stream reader via callback.
}

void SubprocessTransport::OnStderrData(std::span<const uint8_t> /*data*/) {
  // Data is handled by the stream reader via callback.
}

void SubprocessTransport::OnStdoutEof() {
  auto proto = protocol_.lock();
  if (proto) {
    proto->PipeConnectionLost(1, nullptr);
  }
}

void SubprocessTransport::OnStderrEof() {
  auto proto = protocol_.lock();
  if (proto) {
    proto->PipeConnectionLost(2, nullptr);
  }
}

void SubprocessTransport::OnChildExited() {
  int status;
  pid_t result = waitpid(pid_, &status, WNOHANG);
  if (result == 0) {
    // Child still running, check again later.
    loop_.CallLater(std::chrono::milliseconds(10), [this]() { OnChildExited(); });
    return;
  }

  if (result < 0) {
    // waitpid failed. ECHILD means the child has already been reaped
    // (e.g., by Wait()'s own waitpid call). In that case the return_code
    // is already correct — do NOT overwrite it with -1. Keep polling so
    // we eventually converge on the correct state. For other errors
    // (EINVAL, etc.) we also keep polling.
    loop_.CallLater(std::chrono::milliseconds(10), [this]() { OnChildExited(); });
    return;
  }

  exited_ = true;
  if (WIFEXITED(status)) {
    return_code_ = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    return_code_ = -WTERMSIG(status);
  } else {
    return_code_ = -1;
  }

  // Clean up.
  if (stdout_fd_ >= 0) {
    loop_.RemoveReader(stdout_fd_);
    CloseFd(stdout_fd_);
    stdout_fd_ = -1;
  }
  if (stderr_fd_ >= 0) {
    loop_.RemoveReader(stderr_fd_);
    CloseFd(stderr_fd_);
    stderr_fd_ = -1;
  }
  if (stdin_fd_ >= 0) {
    loop_.RemoveWriter(stdin_fd_);
    CloseFd(stdin_fd_);
    stdin_fd_ = -1;
  }

  // Notify protocol.
  auto proto = protocol_.lock();
  if (proto) {
    proto->ProcessExited(return_code_);
  }
}

int64_t SubprocessTransport::Write(std::span<const uint8_t> data) {
  if (stdin_fd_ < 0 || stdin_closed_) {
    return -1;
  }

  if (!stdin_write_pending_) {
    ssize_t n = write(stdin_fd_, data.data(), data.size());
    if (n > 0) {
      if (static_cast<size_t>(n) < data.size()) {
        stdin_write_buffer_.assign(data.begin() + n, data.end());
        stdin_write_pending_ = true;
        loop_.AddWriter(stdin_fd_, [this]() {
          if (stdin_write_buffer_.empty()) {
            stdin_write_pending_ = false;
            loop_.RemoveWriter(stdin_fd_);
            return;
          }
          ssize_t n = write(stdin_fd_, stdin_write_buffer_.data(), stdin_write_buffer_.size());
          if (n > 0) {
            stdin_write_buffer_.erase(stdin_write_buffer_.begin(), stdin_write_buffer_.begin() + n);
            if (stdin_write_buffer_.empty()) {
              stdin_write_pending_ = false;
              loop_.RemoveWriter(stdin_fd_);
            }
          } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            stdin_write_pending_ = false;
            loop_.RemoveWriter(stdin_fd_);
            CloseFd(stdin_fd_);
            stdin_fd_ = -1;
          }
        });
      }
      return n;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      stdin_write_buffer_.assign(data.begin(), data.end());
      stdin_write_pending_ = true;
      loop_.AddWriter(stdin_fd_, [this]() {
        if (stdin_write_buffer_.empty()) {
          stdin_write_pending_ = false;
          loop_.RemoveWriter(stdin_fd_);
          return;
        }
        ssize_t n = write(stdin_fd_, stdin_write_buffer_.data(), stdin_write_buffer_.size());
        if (n > 0) {
          stdin_write_buffer_.erase(stdin_write_buffer_.begin(), stdin_write_buffer_.begin() + n);
          if (stdin_write_buffer_.empty()) {
            stdin_write_pending_ = false;
            loop_.RemoveWriter(stdin_fd_);
          }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
          stdin_write_pending_ = false;
          loop_.RemoveWriter(stdin_fd_);
          CloseFd(stdin_fd_);
          stdin_fd_ = -1;
        }
      });
      return 0;
    } else {
      return -1;
    }
  }

  // Buffer is busy, append to buffer.
  stdin_write_buffer_.insert(stdin_write_buffer_.end(), data.begin(), data.end());
  return data.size();
}

void SubprocessTransport::CloseStdin() {
  if (stdin_fd_ >= 0) {
    loop_.RemoveWriter(stdin_fd_);
    CloseFd(stdin_fd_);
    stdin_fd_ = -1;
  }
  stdin_closed_ = true;
}

void SubprocessTransport::SendSignal(int sig) {
  if (pid_ > 0) {
    ::kill(pid_, sig);
  }
}

void SubprocessTransport::Terminate() {
  SendSignal(SIGTERM);
}

void SubprocessTransport::Kill() {
  SendSignal(SIGKILL);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// SubprocessStreamReader
// ---------------------------------------------------------------------------

SubprocessStreamReader::SubprocessStreamReader(
    EventLoop& loop,
    std::weak_ptr<detail::SubprocessTransport> transport,
    bool is_stderr)
    : loop_(&loop), transport_(transport), is_stderr_(is_stderr) {}

SubprocessStreamReader::~SubprocessStreamReader() = default;

void SubprocessStreamReader::FeedData(std::span<const uint8_t> data) {
  buffer_.insert(buffer_.end(), data.begin(), data.end());
  ResolvePendingReads();
}

void SubprocessStreamReader::FeedEof() {
  eof_ = true;
  ResolvePendingReads();
}

Future<std::vector<uint8_t>> SubprocessStreamReader::Read(size_t n) {
  Future<std::vector<uint8_t>> result;
  if (!buffer_.empty()) {
    size_t to_read = std::min(n, buffer_.size());
    std::vector<uint8_t> out(buffer_.begin(), buffer_.begin() + to_read);
    buffer_.erase(buffer_.begin(), buffer_.begin() + to_read);
    result.SetResult(std::move(out));
  } else if (eof_) {
    result.SetResult(std::vector<uint8_t>{});
  } else {
    pending_reads_.push_back(Future<std::vector<uint8_t>>{});
    pending_reads_.back().AddDoneCallback([this](Future<std::vector<uint8_t>>&) {
      ResolvePendingReads();
    });
  }
  return result;
}

Future<std::vector<uint8_t>> SubprocessStreamReader::ReadExactly(size_t n) {
  Future<std::vector<uint8_t>> result;
  if (buffer_.size() >= n) {
    std::vector<uint8_t> out(buffer_.begin(), buffer_.begin() + n);
    buffer_.erase(buffer_.begin(), buffer_.begin() + n);
    result.SetResult(std::move(out));
  } else if (eof_) {
    result.SetResult(std::vector<uint8_t>{});
  } else {
    pending_reads_.push_back(Future<std::vector<uint8_t>>{});
  }
  return result;
}

Future<std::vector<uint8_t>> SubprocessStreamReader::ReadUntil(std::string_view) {
  // Simplified: return whatever is available.
  Future<std::vector<uint8_t>> result;
  if (!buffer_.empty()) {
    std::vector<uint8_t> out(buffer_.begin(), buffer_.end());
    buffer_.clear();
    result.SetResult(std::move(out));
  } else if (eof_) {
    result.SetResult(std::vector<uint8_t>{});
  } else {
    pending_reads_.push_back(Future<std::vector<uint8_t>>{});
  }
  return result;
}

Future<std::string> SubprocessStreamReader::ReadLine() {
  Future<std::string> result;
  for (size_t i = 0; i < buffer_.size(); ++i) {
    if (buffer_[i] == '\n') {
      std::string out(buffer_.begin(), buffer_.begin() + i + 1);
      buffer_.erase(buffer_.begin(), buffer_.begin() + i + 1);
      result.SetResult(std::move(out));
      return result;
    }
  }
  if (eof_) {
    std::string out(buffer_.begin(), buffer_.end());
    buffer_.clear();
    result.SetResult(std::move(out));
  } else {
    pending_reads_.push_back(Future<std::vector<uint8_t>>{});
  }
  return result;
}

void SubprocessStreamReader::ResolvePendingReads() {
  while (!pending_reads_.empty() && !buffer_.empty()) {
    auto fut = std::move(pending_reads_.front());
    pending_reads_.pop_front();
    size_t to_read = std::min(buffer_.size(), size_t{65536});
    std::vector<uint8_t> out(buffer_.begin(), buffer_.begin() + to_read);
    buffer_.erase(buffer_.begin(), buffer_.begin() + to_read);
    fut.SetResult(std::move(out));
  }
  if (eof_ && pending_reads_.size() == 1 && buffer_.empty()) {
    auto fut = std::move(pending_reads_.front());
    pending_reads_.pop_front();
    fut.SetResult(std::vector<uint8_t>{});
  }
}

// ---------------------------------------------------------------------------
// SubprocessStreamWriter
// ---------------------------------------------------------------------------

SubprocessStreamWriter::SubprocessStreamWriter(
    EventLoop& loop,
    std::weak_ptr<detail::SubprocessTransport> transport)
    : loop_(&loop), transport_(transport) {}

SubprocessStreamWriter::~SubprocessStreamWriter() = default;

void SubprocessStreamWriter::Write(std::span<const uint8_t> data) {
  auto t = transport_.lock();
  if (t && !closed_) {
    t->Write(data);
  }
}

void SubprocessStreamWriter::Close() {
  auto t = transport_.lock();
  if (t && !closed_) {
    t->CloseStdin();
    closed_ = true;
  }
}

// ---------------------------------------------------------------------------
// Subprocess
// ---------------------------------------------------------------------------

Subprocess::Subprocess(std::shared_ptr<detail::SubprocessTransport> transport)
    : transport_(std::move(transport)) {}

Subprocess::~Subprocess() = default;

Future<int64_t> Subprocess::Wait() {
  Future<int64_t> future;
  if (transport_->Exited()) {
    // Already reaped by OnChildExited().
    future.SetResult(transport_->return_code());
    return future;
  }

  auto loop = EventLoop::Current();
  auto transport_sp = transport_;  // local copy, captured by value in poll
  auto pid = transport_->pid();

  std::function<void()> poll;
  // Capture future by ref; copy ts by VALUE (owns transport);
  // also capture poll itself so CallLater can reschedule.
  poll = [&future, loop, pid, ts = transport_sp, &poll]() mutable {
    if (ts->Exited()) {
      future.SetResult(ts->return_code());
      return;
    }
    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result > 0) {
      int64_t code = -1;
      if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        code = -WTERMSIG(status);
      }
      ts->set_return_code(code);
      future.SetResult(code);
    } else if (result == 0) {
      // Child still running — retry after a short delay.
      loop->CallLater(std::chrono::milliseconds(5), poll);
    } else {
      // result < 0: EINTR (retry) or ECHILD (already reaped by
      // OnChildExited). In both cases check Exited() after a delay.
      loop->CallLater(std::chrono::milliseconds(5), poll);
    }
  };

  poll();
  return future;
}

void Subprocess::SendSignal(int sig) {
  if (transport_) transport_->SendSignal(sig);
}

void Subprocess::Terminate() {
  if (transport_) transport_->Terminate();
}

void Subprocess::Kill() {
  if (transport_) transport_->Kill();
}

int Subprocess::pid() const {
  return transport_ ? transport_->pid() : -1;
}

std::optional<int64_t> Subprocess::return_code() const {
  if (!transport_ || !transport_->Exited()) return std::nullopt;
  return transport_->return_code();
}

// ---------------------------------------------------------------------------
// Helper: make a ready future
// ---------------------------------------------------------------------------

namespace {

template <typename T>
Future<T> MakeReadyFuture(T value) {
  Future<T> result;
  result.SetResult(std::move(value));
  return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// create_subprocess_exec
// ---------------------------------------------------------------------------

namespace {

Future<std::unique_ptr<Subprocess>> CreateSubprocessCommon(
    const std::function<pid_t(int, int, int)>& fork_fn,
    int stdin_opt,
    int stdout_opt,
    int stderr_opt,
    EventLoop* loop) {
  // Create pipes.
  auto [stdin_read, stdin_write] = detail::CreatePipe();
  auto [stdout_read, stdout_write] = detail::CreatePipe();
  auto [stderr_read, stderr_write] = detail::CreatePipe();

  if (stdin_read < 0 || stdout_read < 0 || stderr_read < 0) {
    return MakeReadyFuture<std::unique_ptr<Subprocess>>(nullptr);
  }

  // Create sync stdin pipe for communicate().
  auto [sync_stdin_read, sync_stdin_write] = detail::CreatePipe();
  if (sync_stdin_read < 0 || sync_stdin_write < 0) {
    close(stdin_read);
    close(stdin_write);
    close(stdout_read);
    close(stdout_write);
    close(stderr_read);
    close(stderr_write);
    return MakeReadyFuture<std::unique_ptr<Subprocess>>(nullptr);
  }

  pid_t pid = fork_fn(stdin_read, stdout_write, stderr_write);

  if (pid < 0) {
    // Fork failed.
    close(stdin_read);
    close(stdout_read);
    close(stderr_read);
    close(sync_stdin_read);
    close(sync_stdin_write);
    return MakeReadyFuture<std::unique_ptr<Subprocess>>(nullptr);
  }

  if (pid == 0) {
    // Child process.
    // Redirect stdin.
    if (stdin_opt & PIPE_STDIN) {
      dup2(stdin_read, STDIN_FILENO);
    } else {
      int devnull = open("/dev/null", O_RDONLY);
      if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        close(devnull);
      }
    }

    // Redirect stdout.
    if (stdout_opt & PIPE_STDOUT) {
      dup2(stdout_write, STDOUT_FILENO);
    } else {
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        close(devnull);
      }
    }

    // Redirect stderr.
    if (stderr_opt & PIPE_STDERR) {
      dup2(stderr_write, STDERR_FILENO);
    } else {
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
    }

    // Child doesn't need these ends of the pipes.
    close(stdin_read);
    close(stdin_write);
    close(stdout_read);
    close(stdout_write);
    close(stderr_read);
    close(stderr_write);
    close(sync_stdin_read);
    close(sync_stdin_write);
  }

  // Parent process.
  // Close child-side ends.
  close(stdin_read);
  close(stdout_write);
  close(stderr_write);

  if (!(stdin_opt & PIPE_STDIN)) {
    close(stdin_write);
    stdin_write = -1;
  }
  if (!(stdout_opt & PIPE_STDOUT)) {
    close(stdout_read);
    stdout_read = -1;
  }
  if (!(stderr_opt & PIPE_STDERR)) {
    close(stderr_read);
    stderr_read = -1;
  }

  // Create transport.
  auto transport = std::make_shared<detail::SubprocessTransport>(
      *loop,
      std::weak_ptr<SubprocessProtocol>(),
      pid,
      stdin_write,
      stdout_read,
      stderr_read,
      sync_stdin_write);

  // Create subprocess wrapper.
  auto subprocess = std::make_unique<Subprocess>(transport);

  // Start reading from stdout/stderr.
  transport->StartReading();

  return MakeReadyFuture(std::move(subprocess));
}

}  // namespace

Future<std::unique_ptr<Subprocess>> CreateSubprocessExec(
    const std::string& program,
    std::vector<std::string> args,
    int stdin_opt,
    int stdout_opt,
    int stderr_opt,
    EventLoop* loop) {
  if (!loop) loop = EventLoop::Current();

  auto fork_fn = [&program, &args](int /*stdin_fd*/, int /*stdout_fd*/, int /*stderr_fd*/) -> pid_t {
    pid_t pid = fork();
    if (pid != 0) return pid;

    // Child: prepare arguments and exec.
    std::vector<const char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(program.c_str());
    for (auto& arg : args) {
      argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    execvp(program.c_str(), const_cast<char* const*>(argv.data()));
    _exit(127);  // exec failed.
  };

  return CreateSubprocessCommon(fork_fn, stdin_opt, stdout_opt, stderr_opt, loop);
}

Future<std::unique_ptr<Subprocess>> CreateSubprocessShell(
    const std::string& command,
    int stdin_opt,
    int stdout_opt,
    int stderr_opt,
    EventLoop* loop) {
  if (!loop) loop = EventLoop::Current();

  auto fork_fn = [&command](int /*stdin_fd*/, int /*stdout_fd*/, int /*stderr_fd*/) -> pid_t {
    pid_t pid = fork();
    if (pid != 0) return pid;

    // Child: exec shell.
    execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
    _exit(127);  // exec failed.
  };

  return CreateSubprocessCommon(fork_fn, stdin_opt, stdout_opt, stderr_opt, loop);
}

}  // namespace asyncio
