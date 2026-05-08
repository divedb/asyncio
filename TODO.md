# asyncio-cpp TODO

> 用 C++20 完整复刻 Python asyncio 的 API 和语义。
> 进度以 `[ ]` / `[x]` 标记，每个条目注明对应的 Python asyncio 原 API。

---

## 0. 基础设施

### 0.1 构建系统
- [ ] 编写根 `CMakeLists.txt`（C++20 标准，Google Test 集成）
- [ ] 定义 `chibicpp_cc_library()` 风格的 CMake 辅助函数（参考 chibicpp 项目）
- [ ] 配置 CI 矩阵（Linux/gcc, Linux/clang, macOS/apple-clang, Windows/MSVC）

### 0.2 异常类型体系
- [ ] `AsyncError` — 所有 asyncio 异常的基类（`std::runtime_error` 子类）
- [ ] `AsyncCancelledError` — 对应 Python `CancelledError`
- [ ] `AsyncTimeoutError` — 对应 Python `TimeoutError`（asyncio 专用）
- [ ] `InvalidStateError` — Future/Task 状态非法操作
- [ ] `IncompleteReadError` — 流读取未完成（携带 `expected`/`actual`）
- [ ] `BrokenBarrierError` — Barrier 被 abort
- [ ] `QueueShutDownError` — Queue 已关闭
- [ ] `LimitOverrunError` — `ReadUntil` 超出 buffer limit

---

## 1. 高层 API

### 1.1 Runners（运行器）
对应 Python `asyncio.runner` 模块。

- [ ] `asyncio::Run(coro, debug=false)` — 创建事件循环，运行协程至完成，自动关闭循环（对应 `asyncio.run()`）
- [ ] `Runner` 类 — 管理事件循环生命周期，支持多次 `Run()` 调用（对应 `asyncio.Runner`）
  - [ ] `Runner::Runner(debug, loop_factory)`
  - [ ] `Runner::Run(coro)`
  - [ ] `Runner::Close()`
  - [ ] `Runner::GetLoop()`

### 1.2 Coroutines（协程支持）
对应 Python 的 `async def` / `await`。

- [ ] `AsyncTaskPromise<T>` — C++20 协程 Promise 类型
  - [ ] `initial_suspend()` — `suspend_never`（立即开始，对应 Python 协程的 eager start）
  - [ ] `final_suspend()` —  suspend_always`（让 Task 在结束后还能处理回调）
  - [ ] `get_return_object()` — 返回 `Task<T>`
  - [ ] `return_value(T)` / `return_void()` — 设置结果
  - [ ] `unhandled_exception()` — 捕获异常存入 Future
  - [ ] `await_transform(Future<U>&)` — 允许 `co_await future`
- [ ] `Task<T>` 类型别名宏 / 约定 — 函数返回 `Task<T>` 即表示是异步协程

### 1.3 Task（任务）
对应 Python `asyncio.Task`。

- [ ] `Task<T>` 类（继承自 `Future<T>`）
  - [ ] `Task(coro_handle)` — 构造时自动 schedule 第一步
  - [ ] `GetName() / SetName(name)`
  - [ ] `Cancel()` — 请求取消（返回是否已取消）
  - [ ] `Cancelling()` — 当前取消深度（结构化并发）
  - [ ] `Uncancel()` — 减少取消深度
  - [ ] `Done()` — 是否已完成
  - [ ] `Result()` — 获取结果（未完成则抛异常）
  - [ ] `GetException()` — 获取异常指针
- [ ] `CreateTask(coro)` — 创建并调度 Task（对应 `asyncio.create_task()`）
- [ ] `GetCurrentTask()` — 获取当前正在运行的 Task（对应 `asyncio.current_task()`）
- [ ] `GetAllTasks(loop)` — 获取循环中所有 Task（对应 `asyncio.all_tasks()`）
- [ ] `TaskGroup` — 结构化并发原语（对应 `asyncio.TaskGroup`，Python 3.11+）
  - [ ] `TaskGroup::CreateTask(coro)`
  - [ ] `TaskGroup` 析构时等待所有子 Task 完成或取消

### 1.4 等待与组合原语
对应 Python `asyncio.wait`、`asyncio.gather`、`asyncio.shield`、`asyncio.wait_for`。

- [ ] `Gather(Future<T>...)` — 并发运行多个 Task，收集所有结果（对应 `asyncio.gather()`）
  - [ ] `return_exceptions` 模式 — 异常不抛，而是作为结果返回
  - [ ] 外部取消时，所有子 Task 均被取消
- [ ] `Shield(Future<T>)` — 保护内部 Task 不受外部取消影响（对应 `asyncio.shield()`）
- [ ] `Wait(futures, policy, timeout)` — 按策略等待 Future 集合（对应 `asyncio.wait()`）
  - [ ] `WaitPolicy::kFirstCompleted` — 任意一个完成即返回
  - [ ] `WaitPolicy::kFirstException` — 任意一个抛异常即返回
  - [ ] `WaitPolicy::kAllCompleted` — 全部完成才返回
  - [ ] 返回 `WaitResult{done, pending}` 结构体
- [ ] `WaitFor(Future<T>, timeout)` — 带超时的等待（对应 `asyncio.wait_for()`）
  - [ ] 超时时取消目标 Future 并抛 `AsyncTimeoutError`

### 1.5 超时控制
对应 Python `asyncio.timeout`、`asyncio.timeout_at`。

- [ ] `TimeoutScope` — RAII 超时作用域（对应 `asyncio.timeout()`）
  - [ ] 构造时创建定时器，到期则取消当前 Task
  - [ ] 析构时调用 `Uncancel()`
  - [ ] 支持嵌套超时
- [ ] `TimeoutAt(deadline)` — 在指定绝对时间点超时（对应 `asyncio.timeout_at()`）
- [ ] `Sleep(duration)` — 异步睡眠（对应 `asyncio.sleep()`）
  - [ ] `Sleep(0)` 等价于 `Yield()`（让出执行权）
- [ ] `Yield()` — 零延迟让出（对应 `asyncio.sleep(0)`）

### 1.6 Synchronization Primitives（同步原语）
对应 Python `asyncio.sync` 相关类。

#### 1.6.1 AsyncEvent
- [x] `AsyncEvent` 类（对应 `asyncio.Event`）
  - [x] `Set()` — 唤醒所有等待者
  - [x] `Clear()` — 重置为未触发
  - [x] `IsSet()` — 查询状态
  - [x] `Wait()` — 返回 `Future<void>`，Set() 后完成

#### 1.6.2 AsyncLock
- [x] `AsyncLock` 类（对应 `asyncio.Lock`）
  - [x] `Acquire()` — 返回 `Future<Guard>`，获取锁后完成
  - [x] `Release()` — 释放锁，唤醒下一个等待者
  - [x] `Locked()` — 查询是否已锁定
  - [x] `Guard` RAII 类型 — 析构时自动 Release
  - [ ] `ScopedAcquire()` — 返回 `Future<Guard>` 的便捷方法

#### 1.6.3 AsyncSemaphore
- [x] `AsyncSemaphore` 类（对应 `asyncio.Semaphore`）
  - [x] `AsyncSemaphore(initial)` — 构造，设置初始信号量
  - [x] `Acquire()` — 返回 `Future<void>`，信号量 >0 时完成
  - [x] `Release()` — 信号量 +1，唤醒下一个等待者
  - [x] `Locked()` — 查询是否还有可用信号量
- [x] `AsyncBoundedSemaphore` 类（对应 `asyncio.BoundedSemaphore`）
  - [x] 继承 `AsyncSemaphore`，Release 时检查是否超过初始值

#### 1.6.4 AsyncCondition
- [x] `AsyncCondition` 类（对应 `asyncio.Condition`）
  - [x] `AsyncCondition(lock)` — 关联一个 `AsyncLock`
  - [x] `Acquire()` / `Release()` — 代理底层 Lock 的操作
  - [x] `Wait()` — 释放锁 → 等待通知 → 重新获取锁（**取消时也重新获取锁**）
  - [x] `Notify(n=1)` — 唤醒 n 个等待者
  - [x] `NotifyAll()` — 唤醒所有等待者
  - [ ] `WaitFor(duration)` — 带超时的 Wait

#### 1.6.5 AsyncBarrier
- [x] `AsyncBarrier` 类（对应 Python 3.11+ `asyncio.Barrier`）
  - [x] `AsyncBarrier(parties)` — 构造，设置参与方数量
  - [x] `Wait()` — 返回 `Future<void>`，所有参与方都调用后才完成
  - [x] `Abort()` — 中断 Barrier，所有等待者收到 `BrokenBarrierError`
  - [x] `Parties()` / `NWaiting()` — 查询状态
  - [x] `Reset()` — 重置 Barrier（在未 broke 状态下）

#### 1.6.6 AsyncQueue / PriorityQueue / LifoQueue
- [x] `AsyncQueue<T>` 类（对应 `asyncio.Queue`）
  - [x] `AsyncQueue(max_size=0)` — 0 表示无界
  - [x] `Put(item)` — 满时阻塞，返回 `Future<void>`
  - [x] `Get()` — 空时阻塞，返回 `Future<T>`
  - [ ] `PutNowait(item)` — 非阻塞 Put，满时抛 `QueueFull`
  - [ ] `GetNowait()` — 非阻塞 Get，空时抛 `QueueEmpty`
  - [x] `QSize()` — 当前队列大小
  - [x] `Empty()` / `Full()` — 查询状态
  - [x] `TaskDone()` — 标记一个元素已处理（配合 `Join()`）
  - [x] `Join()` — 返回 `Future<void>`，所有 Put 的元素都被 TaskDone 后完成
  - [x] `Shutdown()` — 关闭队列，唤醒所有等待者并抛 `QueueShutDownError`
- [ ] `AsyncPriorityQueue<T>` 类（对应 `asyncio.PriorityQueue`）
  - [ ] 继承/复用 `AsyncQueue`，内部用优先队列
- [ ] `AsyncLifoQueue<T>` 类（对应 `asyncio.LifoQueue`）
  - [ ] 继承/复用 `AsyncQueue`，内部用栈（LIFO）

---

## 2. 异步 I/O（Streams）

对应 Python `asyncio.streams` 模块。

### 2.1 StreamReader
- [ ] `StreamReader` 类（对应 `asyncio.StreamReader`）
  - [ ] `Read(n)` — 读取最多 n 字节，返回 `Future<vector<uint8_t>>`
  - [ ] `ReadUntil(separator)` — 读取直到分隔符，返回 `Future<vector<uint8_t>>`
  - [ ] `ReadExactly(n)` — 精确读取 n 字节（不足则抛 `IncompleteReadError`）
  - [ ] `ReadLine()` — 读取一行（按 `\n` 分隔），返回 `Future<string>`
  - [ ] `AtEof()` — 是否已到 EOF
  - [ ] `SetException(ex)` — 注入错误（transport 层调用）
  - [ ] `FeedData(data)` — 注入数据（protocol 层调用）
  - [ ] `FeedEof()` — 注入 EOF（protocol 层调用）
  - [ ] `SetLimit(limit)` — 设置背压阈值（默认 64KiB）
  - [ ] `IsReadable()` — 当前是否有数据可读

### 2.2 StreamWriter
- [ ] `StreamWriter` 类（对应 `asyncio.StreamWriter`）
  - [ ] `Write(data)` — 非阻塞写入缓冲区
  - [ ] `Writelines(list)` — 依次写入多个 buffer
  - [ ] `Drain()` — 返回 `Future<void>`，缓冲区排空后完成（背压控制）
  - [ ] `Close()` — 关闭写入端
  - [ ] `CanWriteEof()` — 是否支持半关闭
  - [ ] `WriteEof()` — 发送 EOF
  - [ ] `GetExtraInfo(name)` — 获取 transport 元信息（socket 地址等）
  - [ ] `IsClosing()` — 是否正在关闭
  - [ ] `WaitClosed()` — 返回 `Future<void>`，完全关闭后完成

### 2.3 连接工厂函数
- [ ] `OpenConnection(host, port, ssl, ...)` — TCP 客户端连接（对应 `asyncio.open_connection()`）
  - [ ] 返回 `Future<pair<StreamReader, StreamWriter>>`
- [ ] `StartServer(client_handler, host, port, ...)` — TCP 服务端（对应 `asyncio.start_server()`）
  - [ ] 返回 `Future<Server>`（Server 对象支持 `Close()` 和 `WaitClosed()`）
- [ ] `OpenUnixConnection(path, ...)` — Unix Domain Socket 客户端
- [ ] `StartUnixServer(path, ...)` — Unix Domain Socket 服务端

### 2.4 Protocol 和 Transport 集成
- [ ] `StreamProtocol` 类 — 内部使用的 Protocol 实现，桥接 Transport 和 StreamReader/Writer
  - [ ] `ConnectionMade(transport)`
  - [ ] `DataReceived(data)`
  - [ ] `EofReceived()`
  - [ ] `ConnectionLost(exception)`
- [ ] `Server` 类 — 管理服务端生命周期
  - [ ] `Close()`
  - [ ] `WaitClosed()`

---

## 3. 低层 API

### 3.1 Event Loop（事件循环）
对应 Python `asyncio.BaseEventLoop`、`asyncio.AbstractEventLoop`。

- [ ] `EventLoop` 类
  - [ ] **生命周期**
    - [ ] `EventLoop()` — 构造（关联 Selector）
    - [ ] `RunForever()` — 一直运行直到 `Stop()` 被调用（对应 `loop.run_forever()`）
    - [ ] `RunUntilComplete(future)` — 运行直到某个 Future 完成（对应 `loop.run_until_complete()`）
    - [ ] `Stop()` — 停止事件循环（对应 `loop.stop()`）
    - [ ] `IsRunning()` — 是否正在运行
    - [ ] `IsClosed()` — 是否已关闭
    - [ ] `Close()` — 关闭循环，释放 Selector（对应 `loop.close()`）
  - [ ] **回调调度**
    - [ ] `CallSoon(callback)` — 下一个 tick 立即执行（对应 `loop.call_soon()`）
    - [ ] `CallSoonThreadsafe(callback)` — 线程安全的 CallSoon（对应 `loop.call_soon_threadsafe()`）
    - [ ] `CallLater(delay, callback)` — 延迟执行（对应 `loop.call_later()`）
    - [ ] `CallAt(when, callback)` — 在指定绝对时间执行（对应 `loop.call_at()`）
    - [ ] `CallAt(when, callback, ...)` 变参版本
  - [ ] **时间**
    - [ ] `Time()` — 当前事件循环时间（`std::chrono::steady_clock::time_point`）
    - [ ] `Now()` — 同上（别名）
  - [ ] **I/O**
  - [ ] `AddReader(fd, callback)` / `RemoveReader(fd)`
    - [x] 基础实现完成，已集成 Selector 后端
  - [ ] `AddWriter(fd, callback)` / `RemoveWriter(fd)`
    - [x] 基础实现完成，已集成 Selector 后端
    - [ ] `SockRecv(sock, n)` — 异步 socket 接收（对应 `loop.sock_recv()`）
    - [ ] `SockSendall(sock, data)` — 异步 socket 发送（对应 `loop.sock_sendall()`）
    - [ ] `SockConnect(sock, addr)` — 异步 socket 连接（对应 `loop.sock_connect()`）
    - [ ] `SockAccept(sock)` — 异步 socket accept（对应 `loop.sock_accept()`）
    - [ ] `GetAddrInfo(host, ...)` — 异步 DNS 解析（对应 `loop.getaddrinfo()`）
    - [ ] `GetNameInfo(sockaddr, ...)` — 逆向 DNS（对应 `loop.getnameinfo()`）
  - [ ] **子进程（可选，后期里程碑）**
    - [ ] `SubprocessExec(protocol_factory, args, ...)`（对应 `loop.subprocess_exec()`）
    - [ ] `SubprocessShell(protocol_factory, cmd, ...)`（对应 `loop.subprocess_shell()`）
  - [ ] **信号处理（可选）**
    - [ ] `AddSignalHandler(sig, callback)`（对应 `loop.add_signal_handler()`）
    - [ ] `RemoveSignalHandler(sig)`（对应 `loop.remove_signal_handler()`）
  - [ ] **线程池执行器**
    - [ ] `RunInExecutor(func, ...)` — 在线程池中执行阻塞操作（对应 `loop.run_in_executor()`）
    - [ ] `SetDefaultExecutor(executor)`
  - [ ] **调试**
    - [ ] `SetDebug(debug)` / `GetDebug()`
    - [ ] 慢回调警告阈值（对应 Python 的 `slow_callback_duration`）

### 3.2 Handle / TimerHandle
- [ ] `Handle` 类 — 类型擦除的回调包装（对应 Python `_Handle`）
  - [ ] `Cancel()` — 取消调度
  - [ ] `Cancelled()` — 是否已取消
  - [ ] `GetLoop()` — 获取所属事件循环
- [ ] `TimerHandle` 类（继承 `Handle`）— 带绝对截止时间的回调
  - [ ] `When()` — 获取计划执行时间
  - [ ] 定时器堆中按 `When()` 排序

### 3.3 Future（低层）
对应 Python `asyncio.Future`。

- [ ] `Future<T>` 类
  - [ ] **状态查询**
    - [ ] `Done()` — 已完成（result 或 exception 已设置）
    - [ ] `Cancelled()` — 已取消
    - [ ] `Pending()` — 仍在进行中
  - [ ] **结果访问**
    - [ ] `Result()` — 获取结果，未完成或已取消则抛异常
    - [ ] `GetException()` — 获取异常指针
    - [ ] `Exception()` — 获取异常（兼容接口）
  - [ ] **状态设置**
    - [ ] `SetResult(value)` — 设置结果，触发回调
    - [ ] `SetException(exception_ptr)` — 设置异常，触发回调
    - [ ] `SetException(exception)` — 重载，接受异常对象
  - [ ] **取消**
    - [ ] `Cancel()` — 取消 Future（仅当 Pending 时成功）
  - [ ] **回调**
    - [ ] `AddDoneCallback(callback)` — 完成后调用（若已完成则立即调用）
    - [ ] `RemoveDoneCallback(callback)` — 移除回调
  - [ ] **Awaitable 协议**
    - [ ] `operator co_await()` — 返回 `Awaiter` 对象
    - [ ] `Awaiter::await_ready()` / `await_suspend()` / `await_resume()`
  - [ ] `Future<void>` 特化 — 无结果值，仅表示完成/异常
- [ ] `FutureBase` — `Future<T>` 的非模板基类，共享状态管理
- [ ] `WrapFuture(future)` — 将 `std::future` 包装为 `asyncio::Future`（对应 `asyncio.wrap_future()`）
- [ ] `IsFuture(obj)` — 类型检测辅助（对应 `asyncio.isfuture()`）

### 3.4 Transports 和 Protocols（传输层与协议层）
对应 Python `asyncio.transports` 和 `asyncio.protocols`。

#### 3.4.1 Transport 抽象
- [ ] `TransportBase` 类（对应 `asyncio.BaseTransport`）
  - [ ] `Close()` — 关闭传输
  - [ ] `IsClosing()` — 是否正在关闭
  - [ ] `GetExtraInfo(name)` — 获取传输元信息
  - [ ] `SetProtocol(protocol)` / `GetProtocol()`
- [ ] `ReadTransportBase`（对应 `asyncio.ReadTransport`）
  - [ ] `PauseReading()` — 暂停接收数据
  - [ ] `ResumeReading()` — 恢复接收数据
- [ ] `WriteTransportBase`（对应 `asyncio.WriteTransport`）
  - [ ] `Write(data)` — 写入数据
  - [ ] `Writelines(list)` — 写入多个 buffer
  - [ ] `CanWriteEof()`
  - [ ] `WriteEof()`
  - [ ] `Abort()` — 立即关闭，不刷缓冲区
- [ ] `DatagramTransportBase`（对应 `asyncio.DatagramTransport`）
  - [ ] `Sendto(data, addr)` — UDP 发送
- [ ] `SocketTransport` — 基于 socket 的 Transport 实现

#### 3.4.2 Protocol 抽象
- [ ] `ProtocolBase` 类（对应 `asyncio.BaseProtocol`）
  - [ ] `ConnectionMade(transport)`
  - [ ] `ConnectionLost(exception)`
- [ ] `Protocol` 类（对应 `asyncio.Protocol`，TCP 协议）
  - [ ] `DataReceived(data)`
  - [ ] `EofReceived()`
- [ ] `DatagramProtocol` 类（对应 `asyncio.DatagramProtocol`）
  - [ ] `DatagramReceived(data, addr)`
  - [ ] `ErrorReceived(exception)`
- [ ] `SubprocessProtocol` 类（对应 `asyncio.SubprocessProtocol`）
  - [ ] `PipeDataReceived(fd, data)`
  - [ ] `PipeConnectionLost(fd, exception)`
  - [ ] `ProcessExited(returncode)`

#### 3.4.3 创建连接 / 服务端
- [ ] `CreateConnection(protocol_factory, host, port, ...)` — 创建 TCP 连接（对应 `loop.create_connection()`）
- [ ] `CreateServer(protocol_factory, host, port, ...)` — 创建 TCP 服务端（对应 `loop.create_server()`）
- [ ] `CreateDatagramEndpoint(protocol_factory, ...)` — 创建 UDP 端点（对应 `loop.create_datagram_endpoint()`）
- [ ] `CreateUnixConnection(...)` / `CreateUnixServer(...)`
- [ ] `ConnectReadPipe(...)` / `ConnectWritePipe(...)` — 管道连接

### 3.5 Event Loop Policies（事件循环策略）
对应 Python `asyncio.DefaultEventLoopPolicy` 等。

- [ ] `EventLoopPolicy` 抽象类（对应 `asyncio.AbstractEventLoopPolicy`）
  - [ ] `GetEventLoop()` — 获取当前线程的事件循环
  - [ ] `SetEventLoop(loop)` — 设置当前线程的事件循环
  - [ ] `NewEventLoop()` — 创建新的事件循环
  - [ ] `GetChildWatcher()` / `SetChildWatcher()`（子进程监视器，可选）
- [ ] `DefaultEventLoopPolicy` — 默认策略实现
  - [ ] 基于 `thread_local` 存储，每线程一个循环
- [ ] 全局策略管理
  - [ ] `GetEventLoopPolicy()` — 获取全局策略
  - [ ] `SetEventLoopPolicy(policy)` — 设置全局策略
- [ ] `GetEventLoop()` — 便捷函数，获取当前事件循环（对应 `asyncio.get_event_loop()`）
- [ ] `SetEventLoop(loop)` — 便捷函数（对应 `asyncio.set_event_loop()`）
- [ ] `NewEventLoop()` — 便捷函数（对应 `asyncio.new_event_loop()`）
- [ ] `GetRunningLoop()` — 获取正在运行的事件循环，无则返回 null（对应 `asyncio.get_running_loop()`）

---

## 4. 子进程支持（可选 / 后期里程碑）

对应 Python `asyncio.subprocess` 模块。

- [ ] `CreateSubprocessExec(program, args, ...)` — 创建子进程（对应 `asyncio.create_subprocess_exec()`）
  - [ ] 返回 `Future<Process>`
- [ ] `CreateSubprocessShell(cmd, ...)` — 通过 shell 创建子进程（对应 `asyncio.create_subprocess_shell()`）
- [ ] `Process` 类（对应 `asyncio.subprocess.Process`）
  - [ ] `Wait()` — 等待进程结束
  - [ ] `Communicate(input)` — 发送 stdin 并读取 stdout/stderr
  - [ ] `SendSignal(sig)` / `Terminate()` / `Kill()`
  - [ ] `Pid()` — 进程 ID
  - [ ] `Returncode()` — 返回码

---

## 5. 内部实现

### 5.1 事件循环核心
- [ ] `ReadyQueue` — 立即执行的回调队列（`std::deque<Handle>`）
- [ ] `TimerHeap` — 定时器最小堆（`std::vector<TimerHandle>`，按 deadline 排序）
  - [ ] 惰性清理已取消的定时器（取消数 > 堆大小 50% 时重建）
- [ ] `RunOnce()` — 单次事件循环迭代
  - [ ] 清理已取消的定时器
  - [ ] 计算 select 超时时间
  - [ ] 调用 `Selector::Select(timeout)`
  - [ ] 将到期的定时器移入 `ready_`
  - [ ] 执行 `ready_` 中当前批次的所有回调（新加入的推迟到下一批次）
- [ ] Self-Pipe Trick — 跨线程唤醒机制
  - [ ] Linux: `eventfd`
  - [ ] macOS/BSD: `kqueue EVFILT_USER`
  - [ ] 降级: `socketpair()`

### 5.2 Selector 抽象层
- [x] `Selector` 抽象接口（`include/asyncio/detail/selector.h`）
  - [x] `Register(fd, events)` — 注册文件描述符
  - [x] `Modify(fd, events)` — 修改注册事件
  - [x] `Unregister(fd)` — 取消注册（no-op if not registered）
  - [x] `Select(timeout)` — 等待 I/O 事件，返回就绪列表
- [x] `EpollSelector` — Linux epoll 实现
- [x] `KqueueSelector` — macOS/BSD kqueue 实现
- [x] `SelectSelector` — `select()` 可移植降级实现
- [x] `DefaultSelector` — 编译期自动选择最优后端（`selector_backend.h`）
- [ ] `IocpSelector` — Windows IOCP 实现（可选）

### 5.3 Task 驱动逻辑
- [ ] `Task::Step()` — 推进协程执行一步
  - [ ] `coro.resume()` — 恢复协程至下一个悬挂点
  - [ ] `co_return` 分支 → `SetResult()`
  - [ ] `co_await future` 分支 → 在 future 上注册唤醒回调，悬挂
  - [ ] 异常分支 → `SetException()`
  - [ ] 取消分支 → 注入 `AsyncCancelledError`
- [ ] Task 三状态不变式（调试断言）
  - [ ] 状态 1: 正在等待某个 Future（注册了 done callback）
  - [ ] 状态 2: Step 已调度（`Handle` 在 `ready_` 中）
  - [ ] 状态 3: 正在执行（在 `Step()` 内部）

### 5.4 协程工具
- [ ] `CoroutineHandle` 包装 — 安全地持有和释放 `std::coroutine_handle<>`
- [ ] 确保协程帧的生命周期管理正确（避免 use-after-free）

---

## 6. 测试和示例

### 6.1 单元测试（Google Test）
- [ ] `HandleTest` — Handle 取消、TimerHandle 排序
- [ ] `TimerHeapTest` — 堆操作、惰性清理
- [ ] `EventLoopTest` — RunOnce 语义、跨线程 CallSoonThreadsafe
- [ ] `FutureTest` — 状态转换、await 悬挂/恢复、异常传播
- [ ] `TaskTest` — 基本协程执行、co_return、co_await 链式调用
- [ ] `CancellationTest` — 各悬挂点的取消、结构化并发 Uncancel
- [ ] `SleepTest` / `YieldTest` — 定时器精度
- [ ] `GatherTest` — 结果排序、异常模式、取消传播
- [ ] `ShieldTest` — 外部取消不影响内部 Task
- [ ] `WaitTest` / `WaitForTest` — 各策略、超时行为
- [ ] `TimeoutScopeTest` — RAII 取消作用域、嵌套超时
- [x] `AsyncEventTest`
- [x] `AsyncLockTest` — 公平性、RAII Guard
- [x] `AsyncSemaphoreTest`
- [x] `AsyncConditionTest` — 取消时重新获取锁
- [x] `AsyncQueueTest` — 有界/无界、背压、Join、Shutdown
- [x] `AsyncBarrierTest` — 多方等待、Abort、Reset
- [ ] `StreamReaderTest` — Read/ReadUntil/ReadExactly/ReadLine、背压
- [ ] `StreamWriterTest` — Write/Drain、背压控制
- [ ] `ConnectionTest` — 回环 socket 对，echo 测试
- [x] `SelectorTest` — 各后端 Register/Modify/Unregister、select 超时精度、EventLoop I/O 集成

### 6.2 示例程序
- [ ] `hello_coroutine.cc` — 基本协程、co_return、事件循环
- [ ] `echo_server.cc` — StreamReader/StreamWriter、TCP 服务端
- [ ] `gather_demo.cc` — 并发 Task 组合
- [ ] `timeout_demo.cc` — 取消和 TimeoutScope
- [ ] `producer_consumer.cc` — AsyncQueue、同步原语
- [ ] `cancellation_demo.cc` — 结构化并发、TaskGroup 使用

---

## 7. 文档

- [ ] 所有公开 API 添加 Doxygen 注释（描述语义，而不仅是语法）
- [ ] `docs/design.md` — 架构总览，含图表
  - [ ] 事件循环生命周期图
  - [ ] Future/Task 状态机图
  - [ ] I/O 分层架构图（Application → Stream → Protocol → Transport → Selector）
  - [ ] 取消流程序列图
- [ ] `docs/api_reference.md` — 完整 API 参考（按模块组织）
- [ ] `docs/migration_from_python.md` — 从 Python asyncio 迁移指南
- [ ] `docs/differences_with_boost_asio.md` — 与 Boost.Asio 的设计差异

---

## Progress

| 里程碑 | 状态 | 预计完成时间 |
|--------|------|-------------|
| 0. 基础设施 | [ ] 待开始 | |
| 1. 高层 API: Runners, Coroutines, Task | [ ] 待开始 | |
| 2. 高层 API: 等待与组合, 超时 | [ ] 待开始 | |
| 3. 高层 API: 同步原语 | [x] **已完成** | 2026-05-08 |
| 4. 异步 I/O: Streams | [ ] 待开始 | |
| 5. 低层 API: Event Loop, Handle, Future | [ ] 待开始 | |
| 6. 低层 API: Transports/Protocols, Policies | [ ] 待开始 | |
| 7. Selector 抽象层 + 平台后端 | [x] **已完成** | 2026-05-08 |
| 8. 测试 | [ ] 待开始 | |
| 9. 文档 | [ ] 待开始 | |
