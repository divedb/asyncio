#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "asyncio/event_loop.h"
#include "asyncio/future.h"

namespace asyncio {

// ---------------------------------------------------------------------------
// SubprocessProtocol
// ---------------------------------------------------------------------------

/// Protocol for subprocess communication.
/// Mirrors Python's asyncio.SubprocessProtocol.
///
/// Derive from this class and override the notification methods.
/// The protocol is passive — it receives data and notifications from the
/// transport, it does not initiate I/O itself.
class SubprocessProtocol : public std::enable_shared_from_this<SubprocessProtocol> {
 public:
  virtual ~SubprocessProtocol() = default;

  /// Called when the subprocess has started.
  /// @param transport  The SubprocessTransport managing this subprocess.
  virtual void ProcessStarted(class SubprocessTransport& transport) = 0;

  /// Called when the subprocess's pipe to stdin is closed.
  virtual void PipeConnectionLost(int fd, std::exception_ptr exc) = 0;

  /// Called when the subprocess has exited.
  /// @param return_code  The exit code of the subprocess.
  virtual void ProcessExited(int64_t return_code) = 0;
};

// ---------------------------------------------------------------------------
// SubprocessTransport
// ---------------------------------------------------------------------------

namespace detail {

/// Transport that manages a subprocess and its three standard pipes.
/// Mirrors Python's asyncio.SubprocessTransport.
///
/// This transport:
///   - Forks a child process.
///   - Connects the child's stdin/stdout/stderr to pipes.
///   - Monitors the pipes via the event loop.
///   - Notifies the protocol of process lifecycle events.
class SubprocessTransport : public std::enable_shared_from_this<SubprocessTransport> {
 public:
  /// Constructs a subprocess transport.
  ///
  /// @param loop           Event loop to use for I/O scheduling.
  /// @param protocol       Protocol to notify of events.
  /// @param pid            PID of the forked child.
  /// @param stdin_fd       Write end of the stdin pipe (parent side).
  /// @param stdout_fd      Read end of the stdout pipe (parent side).
  /// @param stderr_fd      Read end of the stderr pipe (parent side).
  /// @param sync_stdin_fd  Synchronous stdin fd (for communicate()).
  SubprocessTransport(EventLoop& loop,
                      std::weak_ptr<SubprocessProtocol> protocol,
                      int pid,
                      int stdin_fd,
                      int stdout_fd,
                      int stderr_fd,
                      int sync_stdin_fd);

  ~SubprocessTransport();

  // Non-copyable, non-movable.
  SubprocessTransport(const SubprocessTransport&) = delete;
  SubprocessTransport& operator=(const SubprocessTransport&) = delete;
  SubprocessTransport(SubprocessTransport&&) = delete;
  SubprocessTransport& operator=(SubprocessTransport&&) = delete;

  /// Starts reading from stdout and stderr pipes.
  void StartReading();

  /// Writes data to the subprocess's stdin.
  /// @return Number of bytes written, or -1 on error.
  int64_t Write(std::span<const uint8_t> data);

  /// Closes the stdin pipe of the subprocess.
  void CloseStdin();

  /// Sends a signal to the subprocess.
  void SendSignal(int sig);

  /// Terminates the subprocess (SIGTERM).
  void Terminate();

  /// Kills the subprocess (SIGKILL).
  void Kill();

  /// Returns the subprocess PID.
  [[nodiscard]] int pid() const { return pid_; }

  /// Returns the process exit code.
  /// Valid only after the process has exited.
  [[nodiscard]] int64_t return_code() const { return return_code_; }

  /// Returns true if the subprocess has exited.
  [[nodiscard]] bool Exited() const { return exited_; }

  /// Sets the return code (used by Wait() to record exit status).
  void set_return_code(int64_t code) {
    exited_ = true;
    return_code_ = code;
  }

  /// Callback for stdout data received.
  std::function<void(std::span<const uint8_t>)> stdout_callback() {
    return [this](std::span<const uint8_t> data) { OnStdoutData(data); };
  }

  /// Callback for stderr data received.
  std::function<void(std::span<const uint8_t>)> stderr_callback() {
    return [this](std::span<const uint8_t> data) { OnStderrData(data); };
  }

  /// Callback for stdout EOF.
  std::function<void()> stdout_eof_callback() {
    return [this]() { OnStdoutEof(); };
  }

  /// Callback for stderr EOF.
  std::function<void()> stderr_eof_callback() {
    return [this]() { OnStderrEof(); };
  }

 private:
  void OnStdoutData(std::span<const uint8_t> data);
  void OnStderrData(std::span<const uint8_t> data);
  void OnStdoutEof();
  void OnStderrEof();
  void OnChildExited();
  void CloseFd(int fd);

  EventLoop& loop_;
  std::weak_ptr<SubprocessProtocol> protocol_;

  int pid_ = -1;
  int stdin_fd_ = -1;      // async write end
  int stdout_fd_ = -1;     // async read end
  int stderr_fd_ = -1;    // async read end
  int sync_stdin_fd_ = -1;  // sync write end for communicate()

  bool stdin_closed_ = false;
  bool exited_ = false;
  int64_t return_code_ = -1;

  std::vector<uint8_t> stdin_write_buffer_;
  bool stdin_write_pending_ = false;

  static constexpr int kInvalidFd = -1;
};

}  // namespace detail

// ---------------------------------------------------------------------------
// Subprocess
// ---------------------------------------------------------------------------

/// Represents a subprocess spawned via create_subprocess_exec() or
/// create_subprocess_shell().
/// Mirrors Python's asyncio.subprocess.Process.
///
/// The subprocess is managed by a SubprocessTransport. This class
/// provides the high-level API for interacting with the subprocess.
///
/// Usage:
///   auto proc = co_await CreateSubprocessExec("ls", "-la");
///   co_await proc->Wait();
///
///   auto proc = co_await CreateSubprocessExec("cat", {}, PIPE_STDIN, PIPE_STDOUT, PIPE_STDERR);
class Subprocess {
 public:
  /// Constructs a subprocess wrapping a transport.
  explicit Subprocess(std::shared_ptr<detail::SubprocessTransport> transport);

  ~Subprocess();

  // Non-copyable, non-movable (shared ownership via transport).
  Subprocess(const Subprocess&) = delete;
  Subprocess& operator=(const Subprocess&) = delete;
  Subprocess(Subprocess&&) = delete;
  Subprocess& operator=(Subprocess&&) = delete;

  /// Waits for the subprocess to exit.
  /// @return The exit code of the subprocess.
  Future<int64_t> Wait();

  /// Sends a signal to the subprocess.
  void SendSignal(int sig);

  /// Terminates the subprocess (SIGTERM).
  void Terminate();

  /// Kills the subprocess (SIGKILL).
  void Kill();

  /// Returns the subprocess PID.
  [[nodiscard]] int pid() const;

  /// Returns the exit code. Valid only after Wait() has completed.
  /// Returns nullopt if the process hasn't exited yet.
  [[nodiscard]] std::optional<int64_t> return_code() const;

 private:
  std::shared_ptr<detail::SubprocessTransport> transport_;
};

// ---------------------------------------------------------------------------
// StreamReader for subprocess
// ---------------------------------------------------------------------------

/// Buffered async reader for subprocess stdout/stderr.
/// Mirrors the interface of StreamReader but works with SubprocessTransport.
class SubprocessStreamReader {
 public:
  /// Constructs a subprocess stream reader.
  /// @param transport  The subprocess transport.
  /// @param is_stderr  True if this reads from stderr, false for stdout.
  SubprocessStreamReader(EventLoop& loop,
                         std::weak_ptr<detail::SubprocessTransport> transport,
                         bool is_stderr);

  ~SubprocessStreamReader();

  // Non-copyable.
  SubprocessStreamReader(const SubprocessStreamReader&) = delete;
  SubprocessStreamReader& operator=(const SubprocessStreamReader&) = delete;

  /// Reads up to |n| bytes.
  Future<std::vector<uint8_t>> Read(size_t n);

  /// Reads exactly |n| bytes.
  Future<std::vector<uint8_t>> ReadExactly(size_t n);

  /// Reads until |sep| is found (included in result).
  Future<std::vector<uint8_t>> ReadUntil(std::string_view sep);

  /// Reads a line (until '\n').
  Future<std::string> ReadLine();

  /// Feeds data into the reader (called by transport).
  void FeedData(std::span<const uint8_t> data);

  /// Signals EOF to the reader.
  void FeedEof();

  /// Returns true if EOF has been received.
  [[nodiscard]] bool AtEof() const { return eof_; }

  /// Returns true if there is buffered data.
  [[nodiscard]] size_t BufferSize() const { return buffer_.size(); }

 private:
  void ResolvePendingReads();

  [[maybe_unused]] EventLoop* loop_;
  [[maybe_unused]] std::weak_ptr<detail::SubprocessTransport> transport_;
  [[maybe_unused]] bool is_stderr_;

  std::deque<uint8_t> buffer_;
  std::deque<Future<std::vector<uint8_t>>> pending_reads_;
  bool eof_ = false;
};

// ---------------------------------------------------------------------------
// StreamWriter for subprocess
// ---------------------------------------------------------------------------

/// Buffered async writer for subprocess stdin.
/// Mirrors the interface of StreamWriter but works with SubprocessTransport.
class SubprocessStreamWriter {
 public:
  /// Constructs a subprocess stream writer.
  SubprocessStreamWriter(EventLoop& loop,
                          std::weak_ptr<detail::SubprocessTransport> transport);

  ~SubprocessStreamWriter();

  // Non-copyable.
  SubprocessStreamWriter(const SubprocessStreamWriter&) = delete;
  SubprocessStreamWriter& operator=(const SubprocessStreamWriter&) = delete;

  /// Writes data to stdin.
  void Write(std::span<const uint8_t> data);

  /// Closes stdin.
  void Close();

  /// Returns true if stdin is closed.
  [[nodiscard]] bool IsClosing() const { return closed_; }

 private:
  [[maybe_unused]] EventLoop* loop_;
  std::weak_ptr<detail::SubprocessTransport> transport_;
  bool closed_ = false;
};

// ---------------------------------------------------------------------------
// Pipe configuration constants
// ---------------------------------------------------------------------------

/// Indicates that the subprocess's stdin/stdout/stderr should be
/// connected to a stream that the caller can use.
constexpr int PIPE_STDIN = 1 << 0;

/// Indicates that the subprocess's stdout should be connected to a
/// stream that the caller can use.
constexpr int PIPE_STDOUT = 1 << 1;

/// Indicates that the subprocess's stderr should be connected to a
/// stream that the caller can use.
constexpr int PIPE_STDERR = 1 << 2;

/// Indicates that the subprocess's stdin/stdout/stderr should be
/// connected to the parent process (inherited).
constexpr int DEVNULL_STDIN = 1 << 3;
constexpr int DEVNULL_STDOUT = 1 << 4;
constexpr int DEVNULL_STDERR = 1 << 5;

// ---------------------------------------------------------------------------
// create_subprocess_exec
// ---------------------------------------------------------------------------

/// Creates a subprocess by executing an external program.
///
/// Mirrors Python's asyncio.create_subprocess_exec().
///
/// Usage:
///   auto proc = co_await CreateSubprocessExec("ls", "-la");
///   auto proc = co_await CreateSubprocessExec("cat", {}, PIPE_STDIN, PIPE_STDOUT, PIPE_STDERR);
///
/// @param program  The program to execute.
/// @param args    Arguments to pass to the program.
/// @param stdin   PIPE_STDIN to connect stdin, DEVNULL_STDIN to ignore.
/// @param stdout  PIPE_STDOUT to connect stdout, DEVNULL_STDOUT to ignore.
/// @param stderr  PIPE_STDERR to connect stderr, DEVNULL_STDERR to ignore.
/// @param loop    Event loop to use (defaults to current).
/// @return A Subprocess object.
Future<std::unique_ptr<Subprocess>> CreateSubprocessExec(
    const std::string& program,
    std::vector<std::string> args = {},
    int stdin = PIPE_STDIN,
    int stdout = PIPE_STDOUT,
    int stderr = PIPE_STDERR,
    EventLoop* loop = nullptr);

// ---------------------------------------------------------------------------
// create_subprocess_shell
// ---------------------------------------------------------------------------

/// Creates a subprocess by executing a shell command.
///
/// Mirrors Python's asyncio.create_subprocess_shell().
///
/// Usage:
///   auto proc = co_await CreateSubprocessShell("ls -la");
///
/// @param command  The shell command to execute.
/// @param stdin    PIPE_STDIN to connect stdin, DEVNULL_STDIN to ignore.
/// @param stdout   PIPE_STDOUT to connect stdout, DEVNULL_STDOUT to ignore.
/// @param stderr   PIPE_STDERR to connect stderr, DEVNULL_STDERR to ignore.
/// @param loop     Event loop to use (defaults to current).
/// @return A Subprocess object.
Future<std::unique_ptr<Subprocess>> CreateSubprocessShell(
    const std::string& command,
    int stdin = PIPE_STDIN,
    int stdout = PIPE_STDOUT,
    int stderr = PIPE_STDERR,
    EventLoop* loop = nullptr);

}  // namespace asyncio


