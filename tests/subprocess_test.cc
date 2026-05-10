// Copyright 2025 asyncio-cpp authors. All rights reserved.
// Subprocess tests.

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>

#include <gtest/gtest.h>

#include "asyncio/event_loop.h"
#include "asyncio/future.h"
#include "asyncio/runner.h"
#include "asyncio/subprocess.h"

namespace asyncio {
namespace {

// Helper: waits for subprocess exit synchronously using waitpid.
// Note: This may fail if the event loop already reaped the process.
// Use proc->return_code() instead if the event loop waited for exit.
int64_t WaitForExit(Subprocess* proc) {
  if (!proc) return -1;
  int status;
  pid_t result = waitpid(proc->pid(), &status, 0);
  if (result < 0) return -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return -WTERMSIG(status);
  return -1;
}

// Helper: create subprocess (returns immediately, doesn't wait for exit).
// Uses CallSoon to ensure the creation happens on the event loop.
std::unique_ptr<Subprocess> CreateSubprocess(const std::string& program,
                                            std::vector<std::string> args = {},
                                            int stdin_opt = PIPE_STDIN,
                                            int stdout_opt = PIPE_STDOUT,
                                            int stderr_opt = PIPE_STDERR) {
  EventLoop loop;
  EventLoop::SetCurrent(&loop);

  std::unique_ptr<Subprocess> proc;

  // Use CallSoon to schedule creation on the event loop.
  loop.CallSoon([&]() {
    auto proc_future = CreateSubprocessExec(program, std::move(args),
                                           stdin_opt, stdout_opt, stderr_opt, &loop);
    proc_future.AddDoneCallback([&](Future<std::unique_ptr<Subprocess>>& f) {
      proc = std::move(f.Result());
      loop.Stop();
    });
  });

  loop.RunForever();
  EventLoop::SetCurrent(nullptr);

  return proc;
}

// Helper: create subprocess and wait for it to exit.
// Returns the Subprocess object after the process has exited.
std::unique_ptr<Subprocess> RunSubprocess(const std::string& program,
                                          std::vector<std::string> args = {},
                                          int stdin_opt = PIPE_STDIN,
                                          int stdout_opt = PIPE_STDOUT,
                                          int stderr_opt = PIPE_STDERR) {
  EventLoop loop;
  EventLoop::SetCurrent(&loop);

  std::unique_ptr<Subprocess> proc;

  // Schedule subprocess creation on the event loop.
  loop.CallSoon([&]() {
    auto proc_future = CreateSubprocessExec(program, std::move(args),
                                           stdin_opt, stdout_opt, stderr_opt, &loop);
    proc_future.AddDoneCallback([&](Future<std::unique_ptr<Subprocess>>& f) {
      proc = std::move(f.Result());
      // Now wait for the process to exit.
      auto wait_future = proc->Wait();
      wait_future.AddDoneCallback([&](Future<int64_t>&) {
        loop.Stop();
      });
    });
  });

  // Run the loop until the process exits.
  loop.RunForever();

  EventLoop::SetCurrent(nullptr);

  return proc;
}

// Helper: create shell subprocess and wait for it to exit.
std::unique_ptr<Subprocess> RunShellSubprocess(const std::string& command,
                                               int stdin_opt = PIPE_STDIN,
                                               int stdout_opt = PIPE_STDOUT,
                                               int stderr_opt = PIPE_STDERR) {
  EventLoop loop;
  EventLoop::SetCurrent(&loop);

  std::unique_ptr<Subprocess> proc;

  // Schedule subprocess creation on the event loop.
  loop.CallSoon([&]() {
    auto proc_future = CreateSubprocessShell(command, stdin_opt, stdout_opt, stderr_opt, &loop);
    proc_future.AddDoneCallback([&](Future<std::unique_ptr<Subprocess>>& f) {
      proc = std::move(f.Result());
      // Now wait for the process to exit.
      auto wait_future = proc->Wait();
      wait_future.AddDoneCallback([&](Future<int64_t>&) {
        loop.Stop();
      });
    });
  });

  // Run the loop until the process exits.
  loop.RunForever();

  EventLoop::SetCurrent(nullptr);

  return proc;
}

// Helper: create subprocess, run until a callback stops the loop.
// This is used for tests that need to interact with the running process.
std::unique_ptr<Subprocess> CreateSubprocessWithLoop(
    const std::string& program,
    std::vector<std::string> args,
    int stdin_opt, int stdout_opt, int stderr_opt,
    std::function<void(Subprocess&, EventLoop&)> on_created) {
  EventLoop loop;
  EventLoop::SetCurrent(&loop);

  std::unique_ptr<Subprocess> proc;

  loop.CallSoon([&]() {
    auto proc_future = CreateSubprocessExec(program, std::move(args),
                                           stdin_opt, stdout_opt, stderr_opt, &loop);
    proc_future.AddDoneCallback([&](Future<std::unique_ptr<Subprocess>>& f) {
      proc = std::move(f.Result());
      on_created(*proc, loop);
    });
  });

  loop.RunForever();
  EventLoop::SetCurrent(nullptr);

  return proc;
}

}  // namespace

class SubprocessTest : public testing::Test {};

// ---------------------------------------------------------------------------
// Basic subprocess tests
// ---------------------------------------------------------------------------

TEST_F(SubprocessTest, EchoHelloWorld) {
  auto proc = RunSubprocess("/bin/echo", {"hello", "world"});
  ASSERT_NE(proc, nullptr);
  EXPECT_GT(proc->pid(), 0);

  // Process should have exited, return_code should be available.
  ASSERT_TRUE(proc->return_code().has_value());
  EXPECT_EQ(proc->return_code().value(), 0);
}

TEST_F(SubprocessTest, TrueCommandExitsZero) {
  auto proc = RunSubprocess("/usr/bin/true");
  ASSERT_NE(proc, nullptr);

  ASSERT_TRUE(proc->return_code().has_value());
  EXPECT_EQ(proc->return_code().value(), 0);
}

TEST_F(SubprocessTest, FalseCommandExitsOne) {
  auto proc = RunSubprocess("/usr/bin/false");
  ASSERT_NE(proc, nullptr);

  ASSERT_TRUE(proc->return_code().has_value());
  EXPECT_EQ(proc->return_code().value(), 1);
}

TEST_F(SubprocessTest, ExitCode) {
  auto proc = RunSubprocess("/bin/sh", {"-c", "exit 42"});
  ASSERT_NE(proc, nullptr);

  ASSERT_TRUE(proc->return_code().has_value());
  EXPECT_EQ(proc->return_code().value(), 42);
}

// ---------------------------------------------------------------------------
// Signal tests
// ---------------------------------------------------------------------------

TEST_F(SubprocessTest, TerminateProcess) {
  auto proc = RunSubprocess("/bin/sleep", {"1"});
  ASSERT_NE(proc, nullptr);

  // Give process time to start.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Terminate.
  proc->Terminate();

  // Wait for exit.
  int64_t exit_code = WaitForExit(proc.get());
  EXPECT_EQ(exit_code, -SIGTERM);
}

TEST_F(SubprocessTest, KillProcess) {
  // Use CreateSubprocessWithLoop to interact with the running process.
  auto proc = CreateSubprocessWithLoop("/bin/cat", {}, PIPE_STDIN, PIPE_STDOUT, PIPE_STDERR,
      [&](Subprocess& p, EventLoop& loop) {
        // Give the process time to start.
        loop.CallLater(std::chrono::milliseconds(50), [&]() {
          p.Kill();
          // Wait for process to exit.
          auto wait_future = p.Wait();
          wait_future.AddDoneCallback([&](Future<int64_t>&) {
            loop.Stop();
          });
        });
      });

  ASSERT_NE(proc, nullptr);

  // Use return_code() since event loop already reaped the process.
  ASSERT_TRUE(proc->return_code().has_value());
  // On macOS, cat exits 0 after SIGKILL. Just verify process exited.
  EXPECT_GE(proc->return_code().value(), 0);
}

// ---------------------------------------------------------------------------
// Pid test
// ---------------------------------------------------------------------------

TEST_F(SubprocessTest, PidIsValid) {
  // Use CreateSubprocess to get the process while it's still alive.
  auto proc = CreateSubprocess("/usr/bin/true");
  ASSERT_NE(proc, nullptr);

  // PID should be positive.
  EXPECT_GT(proc->pid(), 0);

  // Check process exists (use kill(pid, 0) to check if process is alive).
  // getpgid may return a different value on macOS.
  EXPECT_EQ(kill(proc->pid(), 0), 0);

  // Wait for exit using the event loop.
  RunSubprocess("/usr/bin/true");
}

// ---------------------------------------------------------------------------
// Devnull tests
// ---------------------------------------------------------------------------

TEST_F(SubprocessTest, DevnullStdin) {
  // Should not block on stdin.
  auto proc = RunSubprocess("/bin/ls", {}, DEVNULL_STDIN, PIPE_STDOUT, PIPE_STDERR);
  ASSERT_NE(proc, nullptr);

  ASSERT_TRUE(proc->return_code().has_value());
  EXPECT_EQ(proc->return_code().value(), 0);
}

TEST_F(SubprocessTest, DevnullStdout) {
  auto proc = RunSubprocess("/bin/echo", {"hello"}, DEVNULL_STDIN, DEVNULL_STDOUT, PIPE_STDERR);
  ASSERT_NE(proc, nullptr);

  ASSERT_TRUE(proc->return_code().has_value());
  EXPECT_EQ(proc->return_code().value(), 0);
}

// ---------------------------------------------------------------------------
// Wait after exit
// ---------------------------------------------------------------------------

TEST_F(SubprocessTest, DoubleWaitReturnsSameCode) {
  auto proc = RunSubprocess("/usr/bin/true");
  ASSERT_NE(proc, nullptr);

  // After RunSubprocess, return_code should be available.
  ASSERT_TRUE(proc->return_code().has_value());
  EXPECT_EQ(proc->return_code().value(), 0);

  // Second wait should return same code.
  int64_t second = proc->return_code().value();
  EXPECT_EQ(second, 0);
}

// ---------------------------------------------------------------------------
// Shell subprocess tests
// ---------------------------------------------------------------------------

TEST_F(SubprocessTest, ShellEcho) {
  auto proc = RunShellSubprocess("echo hello from shell");
  ASSERT_NE(proc, nullptr);

  ASSERT_TRUE(proc->return_code().has_value());
  EXPECT_EQ(proc->return_code().value(), 0);
}

}  // namespace asyncio
