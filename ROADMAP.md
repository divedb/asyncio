# asyncio-cpp — Roadmap

A modern C++20 recreation of Python's `asyncio` library, providing a single-threaded, coroutine-based event loop with cooperative scheduling, async I/O, synchronization primitives, and structured concurrency.

---

## 1. Project Goals and Non-Goals

### Goals

- Faithfully reproduce the **semantics** of Python's asyncio in idiomatic C++20.
- Provide a **single-header-friendly**, reusable library with clean public API boundaries.
- Use **C++20 coroutines** (`co_await` / `co_return` / `co_yield`) as the backbone of all async operations.
- Support multiple **platform I/O backends** (epoll, kqueue, IOCP, select) behind a unified abstraction.
- Include **tests** (Google Test), **examples**, and **documentation** for every major component.
- Follow the **Google C++ Style Guide** strictly.
- Remain **zero-dependency** beyond the C++ standard library and a test framework.

### Non-Goals

- Full compatibility with the Python C API or interoperability with Python coroutines.
- A general-purpose networking framework (no HTTP, DNS, or TLS built in).
- Multi-threaded event loops or work-stealing thread pools (one loop per thread, like Python).
- Real-time or hard-latency guarantees.
- Support for C++ versions before C++20.

---

## 2. Python asyncio ↔ C++ Concept Mapping

| Python asyncio             | C++ Equivalent                                            |
|----------------------------|-----------------------------------------------------------|
| `async def` coroutine      | `Task<T>` returned by a C++20 coroutine (`co_return`)    |
| `await` / `yield`          | `co_await`                                                |
| `asyncio.Future`           | `Future<T>` — shared state with result/exception storage |
| `asyncio.Task`             | `Task<T>` — IS-A `Future<T>`, drives a coroutine handle |
| `asyncio.Event`            | `AsyncEvent`                                              |
| `asyncio.Lock`             | `AsyncLock`                                               |
| `asyncio.Semaphore`        | `AsyncSemaphore`                                          |
| `asyncio.Condition`        | `AsyncCondition`                                          |
| `asyncio.Queue`            | `AsyncQueue<T>`                                           |
| `asyncio.Barrier`          | `AsyncBarrier`                                            |
| `asyncio.sleep`            | `Sleep(duration)`                                         |
| `asyncio.gather`           | `Gather(tasks...)`                                        |
| `asyncio.shield`           | `Shield(task)`                                            |
| `asyncio.wait`             | `Wait(tasks, policy)`                                     |
| `asyncio.wait_for`         | `WaitFor(task, timeout)`                                  |
| `asyncio.timeout`          | `TimeoutScope` (RAII)                                     |
| `asyncio.create_task`      | `CreateTask(coroutine)`                                   |
| `asyncio.StreamReader`     | `StreamReader`                                            |
| `asyncio.StreamWriter`     | `StreamWriter`                                            |
| `asyncio.Protocol`         | `ProtocolBase` (concept / virtual base)                   |
| `asyncio.Transport`        | `TransportBase` (virtual base)                            |
| `CancelledError`           | `AsyncCancelledError` (exception type)                    |
| `Handle`                   | `Handle` — type-erased callback wrapper                   |
| `TimerHandle`              | `TimerHandle` — `Handle` with an absolute deadline        |
| Event loop policy          | `EventLoopPolicy`                                         |
| `call_soon`                | `CallSoon(callback)`                                      |
| `call_later`               | `CallLater(duration, callback)`                           |
| `call_soon_threadsafe`     | `CallSoonThreadsafe(callback)`                            |
| contextvars                | Not replicated (single-threaded; per-task context bag if needed) |

---

## 3. Public API Design

### Namespace

All public names live in `namespace asyncio`. Internal details live in `namespace asyncio::detail`.

### 3.1 Event Loop

```cpp
namespace asyncio {

class EventLoop {
 public:
  // Lifecycle
  static EventLoop& Current();           // Like get_event_loop()
  void RunForever();                      // Like run_forever()
  void RunUntilComplete();                // Runs until no pending tasks
  void Stop();                            // Like stop()
  bool IsRunning() const;
  bool IsClosed() const;
  void Close();

  // Scheduling
  Handle CallSoon(std::function<void()> callback);
  TimerHandle CallLater(std::chrono::nanoseconds delay,
                        std::function<void()> callback);
  TimerHandle CallAt(std::chrono::steady_clock::time_point when,
                     std::function<void()> callback);
  Handle CallSoonThreadsafe(std::function<void()> callback);

  // Task creation
  template <typename T>
  Task<T> CreateTask(coro::coroutine_handle<> coro);

  // Time
  std::chrono::steady_clock::time_point Time() const;
};

}  // namespace asyncio
```

### 3.2 Future

```cpp
namespace asyncio {

template <typename T>
class Future {
 public:
  // State query
  bool Done() const;
  bool Cancelled() const;

  // Result access (blocks semantics check; throws if not done)
  T& Result();
  const T& Result() const;
  std::exception_ptr GetException() const;

  // Resolution
  void SetResult(T value);
  void SetException(std::exception_ptr ex);

  // Cancellation
  bool Cancel();

  // Callbacks
  void AddDoneCallback(std::function<void(Future<T>&)> callback);

  // Awaitable protocol
  struct Awaiter { /* await_ready, await_suspend, await_resume */ };
  Awaiter operator co_await();
};

}  // namespace asyncio
```

### 3.3 Task

```cpp
namespace asyncio {

// Task<T> inherits from Future<T>.
// Constructing a Task from a coroutine handle auto-schedules the first step.
template <typename T>
class Task : public Future<T> {
 public:
  // Cancellation depth (structured concurrency)
  int Cancelling() const;
  void Uncancel();

  std::string GetName() const;
  void SetName(std::string name);
};

}  // namespace asyncio
```

### 3.4 Synchronization Primitives

```cpp
namespace asyncio {

class AsyncEvent {
 public:
  void Set();
  void Clear();
  bool IsSet() const;
  Future<void> Wait();  // Returns a future that completes when Set() is called
};

class AsyncLock {
 public:
  Future<bool> Acquire();
  void Release();
  // RAII guard
  class Guard { /* ... */ };
  Future<Guard> ScopedAcquire();
};

class AsyncSemaphore {
 public:
  explicit AsyncSemaphore(int initial);
  Future<bool> Acquire();
  void Release();
};

class AsyncCondition {
 public:
  Future<void> Wait();
  void Notify(int n = 1);
  void NotifyAll();
};

template <typename T>
class AsyncQueue {
 public:
  explicit AsyncQueue(int max_size = 0);  // 0 = unbounded
  Future<void> Put(T item);
  Future<T> Get();
  void TaskDone();
  Future<void> Join();
  int QSize() const;
  int Empty() const;
  int Full() const;
};

class AsyncBarrier {
 public:
  explicit AsyncBarrier(int parties);
  Future<void> Wait();
  void Abort();
};

}  // namespace asyncio
```

### 3.5 Timers and Sleeping

```cpp
namespace asyncio {

// Resolves after the given duration
Future<void> Sleep(std::chrono::nanoseconds duration);

// Zero-duration sleep yields control (like asyncio.sleep(0))
Future<void> Yield();

}  // namespace asyncio
```

### 3.6 High-Level Composition

```cpp
namespace asyncio {

// Gather: run all tasks concurrently, collect results
template <typename... Ts>
Future<std::tuple<Ts...>> Gather(Future<Ts>&... futures);

// Overload for homogeneous vector
template <typename T>
Future<std::vector<T>> Gather(std::vector<Future<T>> futures);

// Wait: return (done_set, pending_set) based on policy
enum class WaitPolicy { kFirstCompleted, kFirstException, kAllCompleted };
struct WaitResult { /* done, pending */ };
Future<WaitResult> Wait(std::vector<Handle> tasks,
                        std::chrono::nanoseconds timeout,
                        WaitPolicy policy);

// Shield: outer cancellation does not cancel inner task
template <typename T>
Future<T> Shield(Future<T>& inner);

// WaitFor: timeout wrapper
template <typename T>
Future<T> WaitFor(Future<T>& fut, std::chrono::nanoseconds timeout);

// TimeoutScope: RAII cancellation-scope guard
class TimeoutScope {
 public:
  explicit TimeoutScope(std::chrono::nanoseconds duration);
  ~TimeoutScope();  // Calls Uncancel()
};

}  // namespace asyncio
```

### 3.7 Async I/O (Streams)

```cpp
namespace asyncio {

class StreamReader {
 public:
  Future<std::vector<uint8_t>> Read(int n);
  Future<std::vector<uint8_t>> ReadUntil(std::string_view separator);
  Future<std::vector<uint8_t>> ReadExactly(int n);
  Future<std::string> ReadLine();
  bool AtEof() const;
};

class StreamWriter {
 public:
  void Write(std::span<const uint8_t> data);
  Future<void> Drain();
  void Close();
  Future<void> WaitClosed();
};

// Connection factory
Future<std::pair<StreamReader, StreamWriter>> OpenConnection(
    const std::string& host, int port);

Future<std::pair<StreamReader, StreamWriter>> StartServer(
    std::function<void(StreamReader&, StreamWriter&)> client_handler,
    const std::string& host, int port);

}  // namespace asyncio
```

### 3.8 Handle and TimerHandle

```cpp
namespace asyncio {

class Handle {
 public:
  void Cancel();
  bool Cancelled() const;
};

class TimerHandle : public Handle {
 public:
  std::chrono::steady_clock::time_point When() const;
};

}  // namespace asyncio
```

---

## 4. Internal Architecture

### 4.1 Directory Layout

```
asyncio/
├── ROADMAP.md
├── CMakeLists.txt
├── include/
│   └── asyncio/
│       ├── asyncio.h              // Single convenience header
│       ├── event_loop.h
│       ├── future.h
│       ├── task.h
│       ├── handle.h
│       ├── timer_handle.h
│       ├── sleep.h
│       ├── gather.h
│       ├── shield.h
│       ├── wait.h
│       ├── sync/
│       │   ├── event.h
│       │   ├── lock.h
│       │   ├── semaphore.h
│       │   ├── condition.h
│       │   ├── queue.h
│       │   └── barrier.h
│       ├── stream/
│       │   ├── stream_reader.h
│       │   ├── stream_writer.h
│       │   └── connection.h
│       ├── transport/
│       │   ├── transport.h
│       │   └── protocol.h
│       └── detail/
│           ├── event_loop_impl.h
│           ├── selector.h
│           ├── selector_backend.h
│           ├── callback_queue.h
│           ├── timer_heap.h
│           ├── self_pipe.h
│           └── coroutine_utils.h
├── src/
│   ├── event_loop.cc
│   ├── future.cc
│   ├── task.cc
│   ├── sleep.cc
│   ├── gather.cc
│   ├── sync/
│   │   ├── event.cc
│   │   ├── lock.cc
│   │   ├── semaphore.cc
│   │   ├── condition.cc
│   │   ├── queue.cc
│   │   └── barrier.cc
│   ├── stream/
│   │   ├── stream_reader.cc
│   │   ├── stream_writer.cc
│   │   └── connection.cc
│   ├── transport/
│   │   └── transport.cc
│   └── detail/
│       ├── selector_epoll.cc      // Linux
│       ├── selector_kqueue.cc     // macOS / BSD
│       ├── selector_iocp.cc       // Windows
│       ├── selector_select.cc     // Fallback
│       ├── self_pipe.cc
│       └── timer_heap.cc
├── tests/
│   ├── CMakeLists.txt
│   ├── event_loop_test.cc
│   ├── future_test.cc
│   ├── task_test.cc
│   ├── cancellation_test.cc
│   ├── timer_test.cc
│   ├── sleep_test.cc
│   ├── gather_test.cc
│   ├── shield_test.cc
│   ├── wait_test.cc
│   ├── sync/
│   │   ├── event_test.cc
│   │   ├── lock_test.cc
│   │   ├── semaphore_test.cc
│   │   ├── condition_test.cc
│   │   ├── queue_test.cc
│   │   └── barrier_test.cc
│   └── stream/
│       └── stream_test.cc
├── examples/
│   ├── hello_coroutine.cc
│   ├── echo_server.cc
│   ├── gather_demo.cc
│   ├── timeout_demo.cc
│   └── producer_consumer.cc
└── docs/
    └── design.md
```

### 4.2 Key Design Principles

1. **Single-threaded event loop**: All coroutine steps run on the event loop thread. No locks needed for internal state.
2. **Two-queue scheduling**: A ready deque for immediate callbacks and a timer min-heap for delayed callbacks — exactly like Python's `_ready` + `_scheduled`.
3. **Cooperative cancellation**: Cancellation sets a flag and injects `AsyncCancelledError` on the next coroutine step. The coroutine can catch and suppress it.
4. **Type-erased callbacks**: `Handle` wraps `std::function<void()>` for `CallSoon`; `TimerHandle` adds an absolute deadline.
5. **Task IS-A Future**: `Task<T>` publicly inherits from `Future<T>`. The task resolves when its coroutine completes.

---

## 5. Event Loop Design

### 5.1 Core Data Structures

```
┌──────────────────────────────────────────────┐
│                  EventLoop                   │
│                                              │
│  ┌────────────────┐  ┌────────────────────┐  │
│  │ ready_ (deque) │  │ scheduled_ (heap)  │  │
│  │ ────────────── │  │ ────────────────── │  │
│  │ Handle         │  │ TimerHandle        │  │
│  │ Handle         │  │ TimerHandle        │  │
│  │ Handle         │  │ TimerHandle        │  │
│  │ ...            │  │ ...                │  │
│  └────────────────┘  └────────────────────┘  │
│                                              │
│  ┌────────────────┐  ┌────────────────────┐  │
│  │ selector_      │  │ self_pipe_         │  │
│  │ (epoll/kqueue) │  │ (wake mechanism)   │  │
│  └────────────────┘  └────────────────────┘  │
└──────────────────────────────────────────────┘
```

### 5.2 RunOnce Algorithm

Each tick of `RunOnce()` follows the exact Python semantics:

```
1. Clean cancelled timer handles (lazy; rebuild heap if >50% cancelled)
2. Compute select timeout:
     - ready_ non-empty    → timeout = 0  (poll only)
     - scheduled_ non-empty → timeout = min(next_deadline - now, 86400s)
     - both empty          → timeout = ∞  (block)
3. selector_->Select(timeout) → process I/O events → append callbacks to ready_
4. Move expired timers from scheduled_ to ready_
5. n = ready_.size()   ← snapshot BEFORE running
6. Run exactly n callbacks from ready_ (new ones deferred to next tick)
```

### 5.3 Thread Safety

- **`CallSoon()`**: NOT thread-safe. Must be called from the loop thread.
- **`CallSoonThreadsafe()`**: Thread-safe. Uses the self-pipe (or `eventfd` on Linux, `EVFILT_USER` on macOS) to wake the selector. Internally enqueues into `ready_` under a `std::mutex`.
- Everything else: single-threaded access only.

### 5.4 Event Loop Policy

Following Python's design, a global `EventLoopPolicy` singleton controls loop creation:

```cpp
class EventLoopPolicy {
 public:
  virtual ~EventLoopPolicy() = default;
  virtual EventLoop& GetEventLoop() = 0;
  virtual void SetEventLoop(EventLoop* loop) = 0;
  virtual EventLoop& NewEventLoop() = 0;
  static EventLoopPolicy& Get();           // Global policy
  static void Set(EventLoopPolicy* policy);
};
```

The default policy uses `thread_local` storage — one loop per thread.

---

## 6. Coroutine, Task, and Future Model

### 6.1 Future State Machine

```
           ┌──────────┐
           │  PENDING │
           └────┬─────┘
       ┌───────┼───────────┐
       ▼       ▼           ▼
┌────────────┐ ┌───────────┐ ┌────────────┐
│  FINISHED  │ │ FINISHED  │ │ CANCELLED  │
│  (result)  │ │ (exception)│ │            │
└────────────┘ └───────────┘ └────────────┘
```

Transitions are one-way. Once resolved or cancelled, the Future is immutable.

### 6.2 Future Internals

```cpp
template <typename T>
class Future {
  enum class State { kPending, kFinished, kCancelled };
  State state_ = State::kPending;
  std::optional<T> result_;
  std::exception_ptr exception_;
  std::vector<std::function<void(Future<T>&)>> done_callbacks_;
  EventLoop* loop_;
  // Internal awaiter state
  bool blocking_ = false;  // Equivalent to _asyncio_future_blocking
};
```

### 6.3 Task as Coroutine Driver

A `Task<T>` wraps a `std::coroutine_handle<>` and drives it step-by-step:

```
Task created
  │
  ▼
CallSoon(Step)           ← schedule first step
  │
  ▼
Step():
  coro.resume()          ← run until next suspension point
  │
  ├── co_return val      → SetResult(val)  → Future resolved
  ├── co_await future    → Register Wakeup callback on that future, suspend
  ├── throw exception    → SetException(ex)
  └── cancelled          → SetCancelled()
```

The `Task::Awaiter`:

```cpp
struct Awaiter {
  Future<T>& fut;

  bool await_ready() { return fut.Done(); }

  void await_suspend(std::coroutine_handle<> caller) {
    fut.blocking_ = true;
    fut.AddDoneCallback([caller](auto&) { caller.resume(); });
  }

  T await_resume() { return fut.Result(); }
};
```

### 6.4 Task Three-State Invariant

At any point, a Task is in exactly one of:

1. **Waiting on a Future** — a done callback is registered on another Future (`_fut_waiter`).
2. **Step is scheduled** — a `Step()` Handle is in the `ready_` queue.
3. **Currently executing** — inside `Step()`.

This invariant is checked via debug assertions.

### 6.5 Promise Type

Every async coroutine uses a custom promise type:

```cpp
template <typename T>
struct AsyncTaskPromise {
  std::suspend_never initial_suspend() noexcept { return {}; }
  auto final_suspend() noexcept;
  Task<T> get_return_object();
  void return_value(T value);
  void unhandled_exception();

  // Allow co_await of Future<T>
  template <typename U>
  auto await_transform(Future<U>& fut);
};
```

---

## 7. Cancellation and Timeout Semantics

### 7.1 Cancellation Flow

Cancellation is **cooperative** — it is never forced:

```
Task.Cancel()
  │
  ├── Task waiting on a Future?
  │     → Cancel that Future → Future resolves cancelled
  │     → Task's wakeup fires → Step() injects AsyncCancelledError
  │
  ├── Task step is scheduled in ready_?
  │     → Set must_cancel_ flag → next Step() injects AsyncCancelledError
  │
  └── Task is done already?
        → Return false
```

### 7.2 Cancellation in Coroutines

```cpp
Future<int> MyTask() {
  try {
    co_await SomeOperation();
    co_return 42;
  } catch (const AsyncCancelledError&) {
    // Cancellation caught and suppressed — Task resolves normally
    co_return -1;
  }
}
```

If the coroutine does NOT catch `AsyncCancelledError`, the Task itself becomes cancelled.

### 7.3 Structured Concurrency: Cancelling / Uncancelling

Following Python 3.11+ semantics:

```cpp
class Task : public Future<T> {
  int cancel_count_ = 0;   // Depth of cancellation requests

  int Cancelling() const { return cancel_count_; }
  void Uncancel() {
    if (cancel_count_ > 0) --cancel_count_;
    if (cancel_count_ == 0) must_cancel_ = false;
  }
};
```

### 7.4 TimeoutScope (RAII)

```cpp
// Equivalent to Python's `async with asyncio.timeout(delay):`
{
  TimeoutScope scope(std::chrono::seconds(5));
  // If the body takes >5s, the current task is cancelled
  co_await LongOperation();
  // scope destructor calls Uncancel()
}
```

Implemented by creating a timer that calls `Task::Cancel()` after the deadline and adjusting the cancellation depth on scope exit.

### 7.5 Cancellation in Synchronization Primitives

Every sync primitive must maintain its invariant even under cancellation:

| Primitive | On CancelledError                                   |
|-----------|-----------------------------------------------------|
| Lock      | If Future already resolved (lock granted), keep it. Otherwise wake next waiter. |
| Condition | **Always re-acquire the lock**, even on cancellation. |
| Semaphore | If Future already resolved, decrement is valid. Otherwise wake next. |
| Queue     | Remove the waiter Future from the waiters deque.    |
| Barrier   | Mark barrier as broken; all waiters get `BrokenBarrierError`. |

---

## 8. Timer and Scheduling System

### 8.1 TimerHeap

A custom binary min-heap keyed by `std::chrono::steady_clock::time_point`:

```cpp
class TimerHeap {
 public:
  void Push(TimerHandle handle);
  TimerHandle Pop();
  const TimerHandle& Top() const;
  bool Empty() const;

  // Lazy cleanup: rebuild when >50% of entries are cancelled
  void MaybeRebuild();

 private:
  std::vector<TimerHandle> heap_;
  int cancelled_count_ = 0;
};
```

### 8.2 TimerHandle Lifecycle

```
TimerHandle created (callback + absolute deadline)
  │
  ▼
Pushed into TimerHeap
  │
  ├── Cancelled before firing
  │     → Set cancelled_ flag
  │     → Lazy cleanup removes it later
  │
  └── Deadline reached
        → Popped from heap
        → Moved to ready_ queue
        → Executed in next RunOnce() batch
```

### 8.3 Clock

Use `std::chrono::steady_clock` exclusively for all timing (monotonic, never goes backward).

`EventLoop::Time()` returns `std::chrono::steady_clock::now()`.

Maximum select timeout is capped at 86400 seconds (matching Python's `MAXIMUM_SELECT_TIMEOUT`).

---

## 9. Async Socket and Stream Design

### 9.1 Layered Architecture

```
┌─────────────────────────────────────┐
│       Application Coroutine         │
│  co_await reader.Read(1024)         │
│  writer.Write(data); co_await drain │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│    StreamReader / StreamWriter      │
│    (buffered, backpressure)         │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│   Protocol (data callback interface)│
│   Transport (raw I/O, fd owner)     │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│   Selector (epoll / kqueue / IOCP)  │
└─────────────────────────────────────┘
```

### 9.2 Protocol Interface

```cpp
class ProtocolBase {
 public:
  virtual ~ProtocolBase() = default;
  virtual void ConnectionMade(TransportBase& transport) = 0;
  virtual void ConnectionLost(std::exception_ptr ex) = 0;
  virtual void PauseWriting() = 0;
  virtual void ResumeWriting() = 0;
};

class StreamProtocol : public ProtocolBase {
 public:
  virtual void DataReceived(std::span<const uint8_t> data) = 0;
  virtual void EofReceived() = 0;
};

class DatagramProtocol : public ProtocolBase {
 public:
  virtual void DatagramReceived(std::span<const uint8_t> data,
                                const SocketAddress& addr) = 0;
  virtual void ErrorReceived(std::exception_ptr ex) = 0;
};
```

### 9.3 Transport

The transport owns the socket file descriptor and performs actual I/O:

```cpp
class TransportBase {
 public:
  virtual ~TransportBase() = default;
  virtual void Write(std::span<const uint8_t> data) = 0;
  virtual void Close() = 0;
  virtual bool IsClosing() const = 0;
  virtual void SetProtocol(ProtocolBase& protocol) = 0;
  virtual ProtocolBase& GetProtocol() = 0;
};
```

### 9.4 StreamReader Backpressure

```
buffer_size_ > 2 * limit_  →  PauseWriting()   (transport stops reading)
buffer_size_ <= limit_     →  ResumeWriting()   (transport resumes reading)
```

Default `limit_` = 64 KiB. Single-reader enforcement — only one pending read at a time.

### 9.5 StreamWriter Drain Pattern

```cpp
writer.Write(data);        // Non-blocking buffer write
co_await writer.Drain();   // Wait if transport buffer is full
```

---

## 10. Synchronization Primitives

### 10.1 Design Philosophy

All primitives are **cooperative** — they create and await `Future<void>` internally. No OS-level mutexes, condition variables, or futexes. Everything runs on the event loop thread.

All waiter queues are FIFO (`std::deque<Future<void>>`) for fairness.

### 10.2 Implementation Notes

**AsyncLock**:
- Internal: `bool locked_` + `std::deque<Future<void>> waiters_`.
- `Acquire()`: If unlocked and no non-cancelled waiters, lock immediately. Otherwise enqueue a Future and await it.
- `Release()`: Wake the first non-cancelled waiter by resolving its Future.

**AsyncEvent**:
- Internal: `bool set_` + `std::deque<Future<void>> waiters_`.
- `Set()`: Resolve ALL waiters.
- `Wait()`: If already set, return immediately. Otherwise enqueue and await.

**AsyncCondition**:
- Owns an `AsyncLock` reference.
- `Wait()`: Release the lock → await an internal Future → re-acquire the lock (even on cancellation).
- `Notify(n)`: Resolve up to `n` waiters.

**AsyncSemaphore**:
- Internal: `int value_` + `std::deque<Future<void>> waiters_`.
- `Acquire()`: If `value_ > 0`, decrement and return. Otherwise enqueue and await.
- `Release()`: Increment, wake next waiter if any.

**AsyncQueue<T>**:
- Internal: `std::deque<T> items_` + `std::deque<Future<void>> getters_` + `std::deque<std::pair<T, Future<void>*>> putters_`.
- Bounded when `max_size_ > 0`: `Put()` blocks when full.
- `TaskDone()` / `Join()`: Reference-count task completion.

**AsyncBarrier**:
- State machine: `kFilling → kDraining → kFilling` (cycles).
- Last party to arrive triggers `NotifyAll` and resets.
- `Abort()` breaks the barrier permanently.

---

## 11. Error Handling Strategy

### 11.1 Exception Types

```cpp
namespace asyncio {

// Base for all asyncio errors
class AsyncError : public std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Cancellation (maps to Python's CancelledError)
class AsyncCancelledError : public AsyncError {
 public:
  AsyncCancelledError();
};

// Invalid state operations
class InvalidStateError : public AsyncError {
  using AsyncError::AsyncError;
};

// Timeout (maps to Python's TimeoutError)
class AsyncTimeoutError : public AsyncError {
 public:
  AsyncTimeoutError();
};

// Incomplete read
class IncompleteReadError : public AsyncError {
 public:
  IncompleteReadError(int expected, int actual);
};

// Broken barrier
class BrokenBarrierError : public AsyncError {
 public:
  BrokenBarrierError();
};

// Queue shutdown
class QueueShutDownError : public AsyncError {
 public:
  QueueShutDownError();
};

}  // namespace asyncio
```

### 11.2 Exception Propagation

- Exceptions thrown inside a coroutine are captured via `std::exception_ptr` and stored in the Task/Future.
- `Future<T>::Result()` rethrows the stored exception if the Future finished with an error.
- `co_await` on a failed Future rethrows the exception at the await site.
- Unhandled exceptions in `RunOnce()` callbacks are caught and logged; they do not kill the event loop.

### 11.3 Error Handling in RunOnce

```cpp
for (int i = 0; i < ntodo; ++i) {
  auto handle = ready_.front();
  ready_.pop_front();
  try {
    handle.Run();
  } catch (...) {
    // Log the exception, do not propagate
    // This matches Python's behavior of logging and continuing
  }
}
```

---

## 12. Thread-Safety Model

### 12.1 The Rule

> **Almost nothing is thread-safe.** The only thread-safe entry point is `CallSoonThreadsafe()`.

### 12.2 What IS Thread-Safe

| Component                    | Thread-Safe? | Mechanism                     |
|------------------------------|-------------|-------------------------------|
| `CallSoonThreadsafe()`       | Yes         | Mutex + self-pipe wakeup      |
| `Handle::Cancel()` (thread-safe variant) | Yes | Internal mutex        |
| Self-pipe write              | Yes         | Atomic socket write (1 byte)  |

### 12.3 What Is NOT Thread-Safe

Everything else: `Future`, `Task`, `Event`, `Lock`, `Queue`, `CallSoon`, `CallLater`, selector operations, all sync primitives.

### 12.4 Self-Pipe Implementation

The self-pipe wakes the selector when `CallSoonThreadsafe()` enqueues a callback:

```
CallSoonThreadsafe(callback)
  │
  ├── Lock mutex_
  ├── Push callback into thread_safe_queue_
  └── Write '\0' to self_pipe_write_fd_
        │
        ▼
Selector wakes (read end is registered)
  │
  ├── Read and discard wake byte
  └── Drain thread_safe_queue_ into ready_
```

Platform optimizations:
- **Linux**: `eventfd()` instead of socket pair.
- **macOS / BSD**: `kqueue EVFILT_USER` instead of socket pair.
- **Fallback**: `socketpair()` (portable).

---

## 13. Platform Backend Strategy

### 13.1 Selector Abstraction

```cpp
namespace asyncio::detail {

struct IoEvent {
  int fd;
  bool readable;
  bool writable;
};

class Selector {
 public:
  virtual ~Selector() = default;
  virtual void Register(int fd, int events) = 0;
  virtual void Modify(int fd, int events) = 0;
  virtual void Unregister(int fd) = 0;
  virtual std::vector<IoEvent> Select(
      std::optional<std::chrono::nanoseconds> timeout) = 0;
};

}  // namespace asyncio::detail
```

### 13.2 Backend Selection at Compile Time

```cpp
// Auto-detected via preprocessor:
#if defined(__linux__)
  using DefaultSelector = EpollSelector;     // epoll_create1 / epoll_wait
#elif defined(__APPLE__) || defined(__BSD__)
  using DefaultSelector = KqueueSelector;    // kqueue / kevent
#elif defined(_WIN32)
  using DefaultSelector = IocpSelector;      // CreateIoCompletionPort / GetQueuedCompletionStatus
#else
  using DefaultSelector = SelectSelector;    // select() — portable fallback
#endif
```

### 13.3 Backend Comparison

| Backend   | Platform      | Max FDs       | Performance   | Complexity |
|-----------|---------------|---------------|---------------|------------|
| epoll     | Linux         | Unlimited     | Excellent     | Medium     |
| kqueue    | macOS / BSD   | Unlimited     | Excellent     | Medium     |
| IOCP      | Windows       | Unlimited     | Excellent     | High       |
| select    | Any (fallback)| 1024 (FD_SETSIZE) | Poor    | Low        |

### 13.4 IOCP Considerations

IOCP uses a fundamentally different model (completion-based vs. readiness-based). The `Selector` abstraction must accommodate this:

- On IOCP, "Register for write readiness" becomes "post a write operation."
- The transport layer must be IOCP-aware, not just the selector.
- Consider a separate `IoBackend` concept for IOCP that differs from the readiness model.

This is the **most architecturally complex** platform difference and will be addressed in the I/O milestone.

---

## 14. Testing Strategy

### 14.1 Framework

**Google Test** (`gtest`), consistent with the chibicpp project conventions.

### 14.2 Test Categories

| Category            | Scope                                              |
|---------------------|----------------------------------------------------|
| Unit tests          | Individual classes: `Future`, `Handle`, `TimerHeap` |
| Integration tests   | Event loop with tasks: scheduling, cancellation    |
| Sync primitive tests| Lock, Event, Semaphore, Queue, Barrier, Condition  |
| I/O tests           | Loopback socket pairs for stream tests              |
| Composition tests   | Gather, Shield, Wait, WaitFor                      |
| Cancellation tests  | Cancel at every suspension point, structured conc. |
| Thread safety tests | `CallSoonThreadsafe` from multiple threads          |
| Timer precision tests| Verify timers fire within acceptable tolerance     |
| Example programs    | Compile and run as part of CI                      |

### 14.3 Test Infrastructure

```cpp
// Helper to run a coroutine in a test event loop
template <typename T>
T RunTestEventLoop(Task<T> task);

// Helper to advance the event loop by N ticks
void RunTicks(EventLoop& loop, int n);

// Helper to advance simulated time (for timer tests)
void AdvanceTime(EventLoop& loop, std::chrono::nanoseconds delta);
```

### 14.4 CI Matrix

| Compiler         | Platform | Backend  |
|------------------|----------|----------|
| GCC 13+          | Linux    | epoll    |
| Clang 16+        | Linux    | epoll    |
| Apple Clang 15+  | macOS    | kqueue   |
| MSVC 19.35+      | Windows  | IOCP     |

---

## 15. Documentation Strategy

### 15.1 Inline Documentation

- Every public API has a **Doxygen/Javadoc-style comment** above the declaration.
- Comments describe **semantics**, not just syntax — especially differences from Python asyncio where applicable.

### 15.2 Design Document

`docs/design.md` covers:
- Architecture overview with diagrams
- Event loop lifecycle
- Coroutine integration details
- Platform backend differences
- Threading model
- Comparison with Boost.Asio and libuv

### 15.3 Examples

Each example is a self-contained program demonstrating one concept:

| Example                | Demonstrates                                   |
|------------------------|-------------------------------------------------|
| `hello_coroutine.cc`   | Basic coroutine, co_return, event loop          |
| `echo_server.cc`       | StreamReader / StreamWriter, networking          |
| `gather_demo.cc`       | Concurrent task composition                     |
| `timeout_demo.cc`      | Cancellation and TimeoutScope                   |
| `producer_consumer.cc` | AsyncQueue, synchronization                     |

---

## 16. Milestone-by-Milestone Implementation Plan

### Milestone 1: Foundation (Weeks 1–2)

**Goal**: Build the minimum infrastructure to schedule and run callbacks.

- Build system (`CMakeLists.txt`) with C++20, gtest integration
- `Handle` and `TimerHandle` — type-erased callback wrappers
- `TimerHeap` — custom binary min-heap with lazy cancellation cleanup
- `EventLoop` core:
  - Ready deque + scheduled timer heap
  - `CallSoon`, `CallLater`, `CallAt`
  - `RunOnce`, `RunForever`, `Stop`
- `SelfPipe` — cross-thread wakeup mechanism
- `CallSoonThreadsafe`
- **Tests**: Handle cancellation, TimerHeap ordering, RunOnce tick semantics, thread-safe scheduling

### Milestone 2: Future and Awaitable (Weeks 2–3)

**Goal**: Complete Future with coroutine integration.

- `Future<T>` — state machine (Pending / Finished / Cancelled)
- `Future<void>` specialization
- `Future<T>::Awaiter` — `await_ready`, `await_suspend`, `await_resume`
- Done callbacks (`AddDoneCallback`)
- Result and exception storage (`std::optional<T>`, `std::exception_ptr`)
- Exception types: `AsyncCancelledError`, `InvalidStateError`
- `Sleep(duration)` — creates a timer-resolved Future
- `Yield()` — zero-duration sleep (bare reschedule)
- **Tests**: Future state transitions, await suspension/resumption, exception propagation, Sleep timing

### Milestone 3: Task (Weeks 3–5)

**Goal**: Full coroutine driving with cancellation support.

- `Task<T>` — inherits from `Future<T>`, wraps a `std::coroutine_handle<>`
- `AsyncTaskPromise<T>` — custom promise type
  - `initial_suspend` (suspend_never for eager start)
  - `final_suspend`
  - `return_value` / `return_void`
  - `unhandled_exception`
  - `await_transform` for `Future<T>`
- Task three-state invariant (debug assertions)
- `CreateTask` factory
- Cancellation:
  - `Task::Cancel()` — sets `must_cancel_`, cancels awaited Future
  - `AsyncCancelledError` injection on next step
  - `Cancelling()` / `Uncancel()` for structured concurrency
- **Tests**: Basic coroutine execution, co_return values, co_await chaining, cancellation at each suspension point, uncancel

### Milestone 4: High-Level Composition (Weeks 5–6)

**Goal**: Implement all task composition APIs.

- `Gather` — concurrent task collection with exception aggregation
  - `return_exceptions` mode
  - Child task cancellation when Gather is cancelled
- `Shield` — detach-on-cancel wrapper
- `Wait` — first-completed / first-exception / all-completed policies
- `WaitFor` — timeout wrapper using `TimeoutScope`
- `TimeoutScope` — RAII cancellation-scope guard
- `CreateTask` with eager start option
- **Tests**: Gather result ordering, exception modes, Shield isolation, WaitFor timeout behavior, TimeoutScope nesting

### Milestone 5: Synchronization Primitives (Weeks 6–8)

**Goal**: Full suite of cooperative sync primitives.

- `AsyncEvent` — set / clear / wait
- `AsyncLock` — acquire / release / RAII guard
- `AsyncSemaphore` — acquire / release (bounded variant)
- `AsyncCondition` — wait / notify / notify_all (always re-acquires lock on cancel)
- `AsyncQueue<T>` — put / get / task_done / join / shutdown
- `AsyncBarrier` — parties / wait / abort
- Cancellation handling in every primitive (maintain invariants)
- **Tests**: Single-threaded contention simulation, fairness verification, cancellation during acquire, Queue shutdown, Barrier break

### Milestone 6: Selector Backend (Weeks 8–10)

**Goal**: Platform I/O multiplexing abstraction.

- `Selector` abstract interface
- `EpollSelector` — Linux implementation
- `KqueueSelector` — macOS / BSD implementation
- `SelectSelector` — portable fallback
- Compile-time backend selection
- I/O event integration into `RunOnce`
- **Tests**: Register/modify/unregister, select timeout accuracy, edge cases (closed FD, bad FD)

### Milestone 7: Transport, Protocol, and Streams (Weeks 10–13)

**Goal**: Complete async I/O stack.

- `ProtocolBase`, `StreamProtocol`, `DatagramProtocol`
- `TransportBase`, socket transport implementation
- `StreamReader` — buffered reads, backpressure, single-reader enforcement
  - `Read(n)`, `ReadUntil(separator)`, `ReadExactly(n)`, `ReadLine()`
- `StreamWriter` — write + drain pattern, flow control
- `OpenConnection` — async TCP connect
- `StartServer` — async TCP listen + accept loop
- **Tests**: Loopback echo tests, backpressure verification, partial reads/writes, connection errors, EOF handling

### Milestone 8: Polish and Documentation (Weeks 13–14)

**Goal**: Production readiness.

- Complete Doxygen documentation for all public APIs
- Write `docs/design.md`
- Write all example programs
- Performance benchmarks (callback throughput, timer precision)
- Memory leak checks (AddressSanitizer)
- Thread safety audit
- Windows / IOCP support (if in scope; otherwise document as future work)

---

## 17. Risks, Ambiguities, and Compatibility Limitations

### 17.1 Risks

| Risk                                      | Mitigation                                                    |
|-------------------------------------------|---------------------------------------------------------------|
| C++20 coroutine ABI instability           | Pin to a specific compiler version; test on multiple compilers |
| IOCP completion-based model mismatch      | Address IOCP in a dedicated milestone; may require separate transport design |
| Stack overflow in deep coroutine chains   | Document stack depth limits; provide `RunInExecutor` for offloading |
| Timer heap performance with millions of timers | Consider timer wheel alternative for large-scale use        |
| `co_await` in destructors                 | Destructors cannot be coroutines — document cleanup patterns  |

### 17.2 Ambiguities

- **`co_yield` semantics**: Python's `yield` within a coroutine has a specific meaning to the Task driver. In C++, `co_yield` maps to `co_await` of a yield-expression. We may restrict the API to `co_await` only and not expose `co_yield`.
- **Exception type vs. error code**: Python uses `CancelledError` (an exception). C++ could use exceptions or `std::expected`. We choose exceptions for Python parity, but document the `std::expected` alternative.
- **`Future<void>`**: Requires careful design to avoid template specialization divergence. Use a common base class (`FutureBase`) for shared state.
- **Copy semantics**: Python Futures are reference types. C++ Futures should be `std::shared_ptr`-like or use `std::shared_ptr` internally, since multiple awaiters may reference the same Future.
- **Promise type name collision**: C++ uses "promise type" for coroutine promises; asyncio uses "Protocol" for the callback interface. Keep naming distinct.

### 17.3 Compatibility Limitations

- **No `async for` equivalent**: C++ has no direct async iteration protocol. Could be added later with `AsyncGenerator<T>`.
- **No `async with` equivalent**: RAII covers deterministic cleanup, but there is no direct "async destructor." Use `TimeoutScope` pattern.
- **No `contextvars`**: Python's context variable system has no C++ equivalent. A per-task context bag can be added if needed.
- **Signal handling**: Python asyncio integrates with UNIX signals via the event loop. Not in initial scope for C++.
- **Subprocess support**: Python's `asyncio.create_subprocess_exec`. Out of initial scope.
- **DNS resolution**: Python's `loop.getaddrinfo`. Out of initial scope (would require a resolver library).
- **SSL/TLS**: Out of initial scope. Could wrap an external TLS library later.
- **Windows IOCP**: Significant implementation effort. May ship as a later milestone, with `select` as the Windows fallback initially.

### 17.4 Comparison with Existing C++ Async Libraries

| Aspect              | asyncio-cpp                  | Boost.Asio              | libuv (C)              |
|---------------------|------------------------------|-------------------------|------------------------|
| Coroutine model     | C++20 native                 | C++20 + custom awaitables | Callback-based        |
| Threading           | Single-threaded (Python-like)| Multi-threaded          | Single-threaded        |
| I/O backends        | epoll/kqueue/IOCP/select     | epoll/kqueue/IOCP       | epoll/kqueue/IOCP      |
| Dependency          | None                         | Boost                   | libuv                  |
| Cancellation        | Cooperative + scopes         | Per-operation           | Per-handle             |
| Semantic model      | Python asyncio               | Proactor / Reactor      | Reactor                |
