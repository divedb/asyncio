# asyncio-cpp TODO

> 用 C++20 完整复刻 Python asyncio 的 API 和语义。
> 进度以 `[ ]` / `[x]` 标记，每个条目注明对应的 Python asyncio 原 API。

---

## 0. 基础设施

### 0.1 构建系统
- [x] `CMakeLists.txt`（C++20 标准，Google Test 集成）
- [x] CI 配置（Linux/macOS，CMake presets）

### 0.2 异常类型体系
- [x] `AsyncError` — 所有 asyncio 异常的基类（`std::runtime_error` 子类）
- [x] `AsyncCancelledError` — 对应 Python `CancelledError`
- [x] `AsyncTimeoutError` — 对应 Python `TimeoutError`（asyncio 专用）
- [x] `InvalidStateError` — Future/Task 状态非法操作
- [x] `IncompleteReadError` — 流读取未完成（携带 `expected`/`actual`）
- [x] `BrokenBarrierError` — Barrier 被 abort
- [x] `QueueShutDownError` — Queue 已关闭
- [x] `LimitOverrunError` — `ReadUntil` 超出 buffer limit

---

## 1. 高层 API

### 1.1 Runners（运行器）
对应 Python `asyncio.runner` 模块。

- [x] `asyncio::Run(coro_factory)` — 创建事件循环，调用工厂函数启动协程，运行至完成，自动关闭循环（对应 `asyncio.run()`）
- [x] `Runner` 类 — 管理事件循环生命周期，支持多次 `Run()` 调用（对应 `asyncio.Runner`）
  - [x] `Runner::Runner(debug, loop_factory)`
  - [x] `Runner::Run(coro_factory)`
  - [x] `Runner::Close()`
  - [x] `Runner::GetLoop()`

### 1.2 Coroutines（协程支持）
对应 Python 的 `async def` / `await`。

- [x] `AsyncTaskPromise<T>` — C++20 协程 Promise 类型
  - [x] `initial_suspend()` — `suspend_never`（立即开始，对应 Python 协程的 eager start）
  - [x] `final_suspend()` — `suspend_always`（让 Task 在结束后还能处理回调）
  - [x] `get_return_object()` — 返回 `Task<T>`
  - [x] `return_value(T)` / `return_void()` — 设置结果
  - [x] `unhandled_exception()` — 捕获异常存入 Future
  - [x] `await_transform(Future<U>&)` — 允许 `co_await future`
- [x] `Task<T>` 类型别名约定 — 函数返回 `Task<T>` 即表示是异步协程

### 1.3 Task（任务）
对应 Python `asyncio.Task`。

- [x] `Task<T>` 类（继承自 `Future<T>`）
  - [x] `Task(coro_handle)` — 构造时自动 schedule 第一步（eager start）
  - [x] `GetName() / SetName(name)`
  - [x] `Cancel()` — 请求取消（返回是否已取消）
  - [x] `Cancelling()` — 当前取消深度（结构化并发）
  - [x] `Uncancel()` — 减少取消深度
  - [x] `Done()` — 是否已完成
  - [x] `Result()` — 获取结果（未完成则抛异常）
  - [x] `GetException()` — 获取异常指针
- [x] `TaskGroup` — 结构化并发原语（对应 `asyncio.TaskGroup`，Python 3.11+）
  - [x] `TaskGroup::CreateTask(coro)` — 创建子任务
  - [x] `TaskGroup::WaitComplete()` — 等待所有子任务完成
  - [x] `TaskGroup::CancelAll()` — 取消所有子任务
  - [x] 析构时取消所有子任务
  - [x] 异常传播：任一子任务失败自动取消其他任务
- [ ] `GetCurrentTask()` — 获取当前正在运行的 Task（对应 `asyncio.current_task()`）
- [ ] `GetAllTasks(loop)` — 获取循环中所有 Task（对应 `asyncio.all_tasks()`）

### 1.4 等待与组合原语
对应 Python `asyncio.wait`、`asyncio.gather`、`asyncio.shield`、`asyncio.wait_for`。

- [x] `Gather(Future<T>...)` — 并发运行多个 Task，收集所有结果（对应 `asyncio.gather()`）
  - [x] 向量版本 `Gather(std::vector<Future<T>>)`
  - [x] 变参版本 `Gather(Future<Ts>...)` — 异构 tuple 返回
  - [x] `GatherWithExceptions()` — 异常不抛，而是作为结果返回
  - [ ] 外部取消时，所有子 Task 均被取消（需测试验证）
- [x] `Shield(Future<T>)` — 保护内部 Task 不受外部取消影响（对应 `asyncio.shield()`）
- [x] `Wait(futures, policy, timeout)` — 按策略等待 Future 集合（对应 `asyncio.wait()`）
  - [ ] `WaitPolicy::kFirstCompleted` — 任意一个完成即返回
  - [ ] `WaitPolicy::kFirstException` — 任意一个抛异常即返回
  - [ ] `WaitPolicy::kAllCompleted` — 全部完成才返回
  - [ ] 返回 `WaitResult{done, pending}` 结构体
- [x] `WaitFor(Future<T>, timeout)` — 带超时的等待（对应 `asyncio.wait_for()`）
  - [x] 超时时取消目标 Future 并抛 `AsyncTimeoutError`

### 1.5 超时控制
对应 Python `asyncio.timeout`、`asyncio.timeout_at`。

- [x] `TimeoutScope` — RAII 超时作用域（对应 `asyncio.timeout()`）
  - [x] 构造时创建定时器，到期则取消当前 Task
  - [x] 析构时调用 `Uncancel()`
  - [ ] 支持嵌套超时（需测试验证）
- [ ] `TimeoutAt(deadline)` — 在指定绝对时间点超时（对应 `asyncio.timeout_at()`）
- [x] `Sleep(duration)` — 异步睡眠（对应 `asyncio.sleep()`）
  - [x] `Sleep(0)` 等价于 `Yield()`（让出执行权）
- [x] `Yield()` — 零延迟让出（对应 `asyncio.sleep(0)`）

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
  - [x] `Acquire()` — 返回 `Future<void>`，获取锁后完成
  - [x] `Release()` — 释放锁，唤醒下一个等待者
  - [x] `Locked()` — 查询是否已锁定
  - [x] `LockGuard` RAII 类型 — 析构时自动 Release
  - [ ] `ScopedAcquire()` — 返回 `Future<LockGuard>` 的便捷方法

#### 1.6.3 AsyncSemaphore
- [x] `AsyncSemaphore` 类（对应 `asyncio.Semaphore`）
  - [x] `AsyncSemaphore(initial)` — 构造，设置初始信号量
  - [x] `Acquire()` — 返回 `Future<void>`，信号量 >0 时完成
  - [x] `Release()` — 信号量 +1，唤醒下一个等待者
  - [x] `Value()` — 查询当前信号量值
- [x] `AsyncBoundedSemaphore` 类（对应 `asyncio.BoundedSemaphore`）
  - [x] 继承 `AsyncSemaphore`，Release 时检查是否超过初始值

#### 1.6.4 AsyncCondition
- [x] `AsyncCondition` 类（对应 `asyncio.Condition`）
  - [x] `AsyncCondition(lock)` — 关联一个 `AsyncLock`
  - [x] `Acquire()` / `Release()` — 代理底层 Lock 的操作
  - [x] `Locked()` — 查询底层锁状态
  - [x] `Wait()` — 释放锁 -> 等待通知 -> 重新获取锁（取消时也重新获取锁）
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
  - [x] `Get()` — 空时阻塞，返回 `Task<T>`
  - [ ] `PutNowait(item)` — 非阻塞 Put，满时抛 `QueueFull`
  - [ ] `GetNowait()` — 非阻塞 Get，空时抛 `QueueEmpty`
  - [x] `Size()` — 当前队列大小
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
- [x] `StreamReader` 类（对应 `asyncio.StreamReader`）
  - [x] `Read(n)` — 读取最多 n 字节，返回 `Future<vector<uint8_t>>`
  - [x] `ReadExactly(n)` — 精确读取 n 字节（不足则抛 `IncompleteReadError`）
  - [x] `ReadUntil(separator)` — 读取直到分隔符，返回 `Future<vector<uint8_t>>`
  - [x] `ReadLine()` — 读取一行（按 `\n` 分隔），返回 `Future<string>`
  - [x] `AtEof()` — 是否已到 EOF
  - [x] `FeedData(data)` — 注入数据（protocol 层调用）
  - [x] `FeedEof()` — 注入 EOF（protocol 层调用）
  - [x] `SetException(ex)` — 注入错误（transport 层调用）
  - [x] `SetLimit(limit)` — 设置背压阈值（默认 64KiB）
  - [x] `MaybePause()` — 缓冲区超过 2x 限制时返回 true
  - [x] `Pause()` / `Resume()` — 暂停/恢复接收
  - [x] `IsReadable()` — 当前是否有数据可读
  - [x] `IsPaused()` — 是否已暂停

### 2.2 StreamWriter
- [x] `StreamWriter` 类（对应 `asyncio.StreamWriter`）
  - [x] `Write(data)` — 非阻塞写入缓冲区
  - [x] `Write(string_view)` — 字符串便捷重载
  - [x] `Writelines(list)` — 依次写入多个 buffer
  - [x] `Drain()` — 返回 `Future<void>`，缓冲区排空后完成（背压控制）
  - [x] `Close()` — 关闭写入端
  - [x] `CanWriteEof()` — 是否支持半关闭（始终返回 true）
  - [x] `WriteEof()` — 发送 EOF
  - [x] `GetExtraInfo(name)` — 获取 transport 元信息（socket 地址等）
  - [x] `IsClosing()` — 是否正在关闭
  - [x] `WaitClosed()` — 返回 `Future<void>`，完全关闭后完成

### 2.3 连接工厂函数
- [x] `OpenConnection(host, port, loop)` — TCP 客户端连接（对应 `asyncio.open_connection()`）
  - [x] 返回 `Future<tuple<StreamReader*, StreamWriter*>>`
  - [ ] SSL/TLS 支持
- [x] `StartServer(client_handler, host, port, loop)` — TCP 服务端（对应 `asyncio.start_server()`）
  - [x] 返回 `Future<unique_ptr<Server>>`
  - [ ] SSL/TLS 支持
- [ ] `OpenUnixConnection(path, ...)` — Unix Domain Socket 客户端
- [ ] `StartUnixServer(path, ...)` — Unix Domain Socket 服务端

### 2.4 Server 类
- [x] `Server` 类 — 管理服务端生命周期
  - [x] `Close()` — 关闭服务器套接字
  - [x] `WaitClosed()` — 等待完全关闭
  - [x] `Port()` — 获取监听端口
  - [x] `ConnectionCount()` — 获取活跃连接数
  - [x] `CloseAllConnections()` — 立即关闭所有客户端连接

### 2.5 Protocol 和 Transport 集成
- [x] `StreamProtocol` 类 — 内部使用的 Protocol 实现，桥接 Transport 和 StreamReader/Writer
  - [x] `ConnectionMade(transport)`
  - [x] `DataReceived(data)`
  - [x] `ConnectionLost(exception)`
  - [x] `PauseWriting()` / `ResumeWriting()` — 背压回调
- [x] `SocketTransport` — 基于 socket 的 Transport 实现
  - [x] 读写就绪回调注册
  - [x] 读写缓冲管理
  - [x] 半关闭（WriteEof）
  - [x] Drain/Close/WaitClosed 生命周期
  - [x] PauseReading/ResumeReading
  - [x] `Accept()` 静态方法用于服务端接受连接

---

## 3. 低层 API

### 3.1 Event Loop（事件循环）
对应 Python `asyncio.BaseEventLoop`、`asyncio.AbstractEventLoop`。

- [x] `EventLoop` 类
  - [x] **生命周期**
    - [x] `EventLoop()` — 构造（关联 Selector）
    - [x] `RunForever()` — 一直运行直到 `Stop()` 被调用（对应 `loop.run_forever()`）
    - [x] `RunOnce()` — 单次迭代（对应 `loop.run_once()`）
    - [x] `Stop()` — 停止事件循环（对应 `loop.stop()`）
    - [x] `IsRunning()` — 是否正在运行
    - [x] `Close()` — 关闭循环，释放 Selector（对应 `loop.close()`）
    - [x] `Current()` — 获取当前线程的事件循环（静态）
  - [x] **回调调度**
    - [x] `CallSoon(callback)` — 下一个 tick 立即执行（对应 `loop.call_soon()`）
    - [x] `CallSoonThreadsafe(callback)` — 线程安全的 CallSoon（对应 `loop.call_soon_threadsafe()`）
    - [x] `CallLater(delay, callback)` — 延迟执行（对应 `loop.call_later()`）
    - [x] `CallAt(when, callback)` — 在指定绝对时间执行（对应 `loop.call_at()`）
  - [x] **时间**
    - [x] `Time()` — 当前事件循环时间（`std::chrono::steady_clock::time_point`）
  - [x] **I/O 基础**
    - [x] `AddReader(fd, callback)` / `RemoveReader(fd)` — 注册/取消读就绪回调
    - [x] `AddWriter(fd, callback)` / `RemoveWriter(fd)` — 注册/取消写就绪回调
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
- [x] `Handle` 类 — 类型擦除的回调包装（对应 Python `_Handle`）
  - [x] `Run()` — 执行回调
  - [x] `Cancel()` — 取消调度
  - [x] `Cancelled()` — 是否已取消
  - [x] `Valid()` — 是否持有有效状态
- [x] `TimerHandle` 类（继承 `Handle`）— 带绝对截止时间的回调
  - [x] `When()` — 获取计划执行时间
  - [x] `operator<` / `operator>` — 堆排序比较

### 3.3 Future（低层）
对应 Python `asyncio.Future`。

- [x] `Future<T>` 类
  - [x] **状态查询**
    - [x] `Done()` — 已完成（result 或 exception 已设置）
    - [x] `Cancelled()` — 已取消
  - [x] **结果访问**
    - [x] `Result()` — 获取结果，未完成或已取消则抛异常
    - [x] `GetException()` — 获取异常指针
  - [x] **状态设置**
    - [x] `SetResult(value)` — 设置结果，触发回调
    - [x] `SetException(exception_ptr)` — 设置异常，触发回调
  - [x] **取消**
    - [x] `Cancel()` — 取消 Future（仅当 Pending 时成功）
  - [x] **回调**
    - [x] `AddDoneCallback(callback)` — 完成后调用（若已完成则立即调用）
  - [x] **Awaitable 协议**
    - [x] `operator co_await()` — 返回 Awaiter 对象，支持 `co_await future`
  - [x] `Future<void>` 特化 — 无结果值，仅表示完成/异常
- [ ] `WrapFuture(future)` — 将 `std::future` 包装为 `asyncio::Future`（对应 `asyncio.wrap_future()`）
- [ ] `IsFuture(obj)` — 类型检测辅助（对应 `asyncio.isfuture()`）

### 3.4 Transports 和 Protocols（传输层与协议层）
对应 Python `asyncio.transports` 和 `asyncio.protocols`。

#### 3.4.1 Transport 抽象
- [x] `TransportBase` 类（对应 `asyncio.BaseTransport`）
  - [x] `IsClosing()` — 是否正在关闭
  - [x] `Abort()` — 立即关闭，不刷缓冲区
  - [x] `GetExtraInfo(name)` — 获取传输元信息
- [x] `SocketTransport` — 基于 socket 的 Transport 实现
  - [x] 读/写/半关闭生命周期
  - [x] Pause/Resume reading
  - [x] Drain/Close/WaitClosed
  - [x] 读就绪/写就绪事件处理
  - [ ] SSL/TLS 包装 Transport

#### 3.4.2 Protocol 抽象
- [x] `ProtocolBase` 类（对应 `asyncio.BaseProtocol`）
  - [x] `ConnectionMade(transport)`
  - [x] `DataReceived(data)`
  - [x] `ConnectionLost(exception)`
  - [x] `PauseWriting()` / `ResumeWriting()` — 背压回调
- [x] `StreamProtocol` — TCP 流协议实现
- [ ] `DatagramProtocol` 类（对应 `asyncio.DatagramProtocol`）
  - [ ] `DatagramReceived(data, addr)`
  - [ ] `ErrorReceived(exception)`
- [ ] `SubprocessProtocol` 类（对应 `asyncio.SubprocessProtocol`）
  - [ ] `PipeDataReceived(fd, data)`
  - [ ] `PipeConnectionLost(fd, exception)`
  - [ ] `ProcessExited(returncode)`

#### 3.4.3 创建连接 / 服务端
- [x] `OpenConnection(...)` — 创建 TCP 连接（对应高层 API）
- [x] `StartServer(...)` — 创建 TCP 服务端（对应高层 API）
- [ ] `CreateDatagramEndpoint(protocol_factory, ...)` — 创建 UDP 端点（对应 `loop.create_datagram_endpoint()`）
- [ ] `CreateUnixConnection(...)` / `CreateUnixServer(...)`
- [ ] `ConnectReadPipe(...)` / `ConnectWritePipe(...)` — 管道连接

### 3.5 Event Loop Policies（事件循环策略）
对应 Python `asyncio.DefaultEventLoopPolicy` 等。

- [x] `EventLoopPolicy` 抽象类（对应 `asyncio.AbstractEventLoopPolicy`）
  - [x] `GetEventLoop()` — 获取当前线程的事件循环
  - [x] `SetEventLoop(loop)` — 设置当前线程的事件循环
  - [x] `NewEventLoop()` — 创建新的事件循环
  - [x] `GetRunningLoop()` — 获取正在运行的事件循环
  - [x] `SetRunningLoop()` — 设置正在运行的事件循环
- [x] `DefaultEventLoopPolicy` — 默认策略实现
  - [x] 基于 `thread_local` 存储，每线程一个循环
- [x] 全局策略管理
  - [x] `GetEventLoopPolicy()` — 获取全局策略
  - [x] `SetEventLoopPolicy(policy)` — 设置全局策略
- [x] `GetEventLoop()` — 便捷函数，获取当前事件循环（对应 `asyncio.get_event_loop()`）
- [x] `SetEventLoop(loop)` — 便捷函数（对应 `asyncio.set_event_loop()`）
- [x] `NewEventLoop()` — 便捷函数（对应 `asyncio.new_event_loop()`）
- [x] `GetRunningLoop()` — 获取正在运行的事件循环，无则返回 null（对应 `asyncio.get_running_loop()`）

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
- [x] `ReadyQueue` — 立即执行的回调队列（`std::deque<Handle>`）
- [x] `TimerHeap` — 定时器最小堆（`std::vector<TimerHandle>`，按 deadline 排序）
  - [x] 惰性清理已取消的定时器（取消数 > 堆大小 50% 时重建）
- [x] `RunOnce()` — 单次事件循环迭代
  - [x] 清理已取消的定时器
  - [x] 计算 select 超时时间
  - [x] 调用 `Selector::Select(timeout)`
  - [x] 将到期的定时器移入 `ready_`
  - [x] 执行 `ready_` 中当前批次的所有回调（新加入的推迟到下一批次）
- [x] Self-Pipe Trick — 跨线程唤醒机制
  - [x] Linux: `eventfd`
  - [x] macOS/BSD: `kqueue EVFILT_USER`
  - [x] 降级: `socketpair()`

### 5.2 Selector 抽象层
- [x] `Selector` 抽象接口（`include/asyncio/detail/selector.h`）
  - [x] `Register(fd, events)` — 注册文件描述符
  - [x] `Modify(fd, events)` — 修改注册事件
  - [x] `Unregister(fd)` — 取消注册
  - [x] `Select(timeout)` — 等待 I/O 事件，返回就绪列表
- [x] `EpollSelector` — Linux epoll 实现
- [x] `KqueueSelector` — macOS/BSD kqueue 实现
- [x] `SelectSelector` — `select()` 可移植降级实现
- [x] `DefaultSelector` — 编译期自动选择最优后端（`selector_backend.h`）
- [ ] `IocpSelector` — Windows IOCP 实现

### 5.3 Task 驱动逻辑
- [x] Task 三状态不变式（`TaskState` 持有 `coro_handle` 和 `cancel_awaited_fn`）
  - [x] `AwaitTransform` 拦截 `co_await Future`，注册取消回调
  - [x] `final_suspend()` 延迟协程销毁
- [x] `CreateTask()` 工厂函数

### 5.4 协程工具
- [x] `CoroutineHandle` 包装通过 `TaskState` 管理生命周期
- [x] 协程帧的生命周期由 `TaskState` 和 `final_suspend()` 正确管理

---

## 6. 测试和示例

### 6.1 单元测试（Google Test）
- [x] `HandleTest` — Handle 取消
- [x] `TimerHeapTest` — 堆操作、惰性清理
- [x] `EventLoopTest` — RunOnce 语义、跨线程 CallSoonThreadsafe
- [x] `FutureTest` — 状态转换、await 悬挂/恢复、异常传播
- [x] `TaskTest` — 基本协程执行、co_return、co_await 链式调用
- [ ] `CancellationTest` — 各悬挂点的取消、结构化并发 Uncancel
- [x] `SleepTest` — Sleep/Yield 定时器精度
- [x] `GatherTest` — 结果收集、异常模式
- [x] `ShieldTest` — 外部取消不影响内部 Task
- [x] `WaitTest` — Wait/WaitFor 超时行为
- [ ] `TimeoutScopeTest` — RAII 取消作用域、嵌套超时
- [x] `AsyncEventTest`
- [x] `AsyncLockTest` — 公平性、RAII Guard
- [x] `AsyncSemaphoreTest`
- [x] `AsyncConditionTest` — 取消时重新获取锁
- [x] `AsyncQueueTest` — 有界/无界、背压、Join、Shutdown
- [x] `AsyncBarrierTest` — 多方等待、Abort、Reset
- [x] `StreamReaderTest` — Read/ReadUntil/ReadExactly/ReadLine、背压（部分）
- [ ] `StreamWriterTest` — Write/Drain、背压控制
- [ ] `ConnectionTest` — OpenConnection/StartServer 集成测试
- [x] `SelectorTest` — 各后端 Register/Modify/Unregister、select 超时精度

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
  - [ ] I/O 分层架构图（Application -> Stream -> Protocol -> Transport -> Selector）
  - [ ] 取消流程序列图
- [ ] `docs/api_reference.md` — 完整 API 参考（按模块组织）
- [ ] `docs/migration_from_python.md` — 从 Python asyncio 迁移指南
- [ ] `docs/differences_with_boost_asio.md` — 与 Boost.Asio 的设计差异

---

## 8. 缺失功能汇总（对标 Python asyncio 模块）

以下按 Python asyncio 模块整理的**缺失或未完成**功能清单：

### 8.1 `asyncio` 顶层函数
- [ ] `Run(coro)` — 最顶层入口（对应 `asyncio.run()`）
- [ ] `GetEventLoop()` — 获取当前线程的事件循环
- [ ] `SetEventLoop(loop)` — 设置当前线程的事件循环
- [ ] `NewEventLoop()` — 创建新事件循环
- [ ] `GetRunningLoop()` — 获取正在运行的事件循环
- [ ] `GetEventLoopPolicy()` / `SetEventLoopPolicy(policy)` — 全局策略管理
- [ ] `GetCurrentTask()` — 获取当前 Task
- [ ] `GetAllTasks(loop)` — 获取所有 Task
- [ ] `Wait(futures, timeout, return_when)` — 等待多个 Future
- [ ] `Gather(*coros_or_futures, return_exceptions)` — 并发组合

### 8.2 `asyncio.runner`
- [ ] `Run(coro, debug)` — 创建循环 -> 运行协程 -> 关闭循环
- [ ] `Runner` 类（可复用运行器）

### 8.3 `asyncio.Task`
- [ ] `TaskGroup` — 结构化并发（Python 3.11+）
- [ ] `AllTasks(loop)` — 返回所有 Task 集合

### 8.4 `asyncio.timeout`
- [ ] `TimeoutAt(deadline)` — 绝对时间超时的 RAII 作用域

### 8.5 `asyncio.sync`
- [ ] `AsyncLock::ScopedAcquire()` — RAII 便捷方法
- [ ] `AsyncCondition::WaitFor(duration)` — 带超时的等待
- [ ] `AsyncQueue::PutNowait()` / `GetNowait()` — 非阻塞操作
- [ ] `AsyncPriorityQueue<T>` — 优先级队列
- [ ] `AsyncLifoQueue<T>` — LIFO 队列

### 8.6 `asyncio.streams`
- [ ] SSL/TLS 支持（`open_ssl_connection`, `start_tls`）
- [ ] Unix Domain Socket 支持

### 8.7 `asyncio.base_events` 低层 I/O
- [ ] `SockRecv(sock, n)` — 异步 socket 接收
- [ ] `SockSendall(sock, data)` — 异步 socket 发送
- [ ] `SockConnect(sock, addr)` — 异步 socket 连接
- [ ] `SockAccept(sock)` — 异步 socket 接受
- [ ] `GetAddrInfo(host, port, family, type, proto, flags)` — 异步 DNS
- [ ] `GetNameInfo(sockaddr, flags)` — 逆向 DNS

### 8.8 `asyncio.subprocess`
- [ ] `CreateSubprocessExec/Shell` — 创建子进程
- [ ] `Process` 类 — 进程管理

### 8.9 `asyncio.events` 信号和执行器
- [ ] `AddSignalHandler(sig, callback)` — 信号处理
- [ ] `RemoveSignalHandler(sig)` — 移除信号处理
- [ ] `RunInExecutor(executor, func, *args)` — 线程池执行
- [ ] `DefaultExecutor` — 默认执行器

### 8.10 平台支持
- [ ] Windows IOCP 后端（`IocpSelector`）
- [ ] `asyncio.WindowsProactorEventLoopPolicy`（Windows 特有）

---

## Progress

| 里程碑 | 状态 | 完成日期 |
|--------|------|----------|
| 0. 基础设施 | [x] **已完成** | 2026-05-08 |
| 1. 异常类型体系 | [x] **已完成** | 2026-05-08 |
| 2. Coroutines + Task | [x] **已完成** | 2026-05-08 |
| 3. 高层 API: Gather/Shield/Wait/WaitFor | [x] **已完成** | 2026-05-08 |
| 4. 高层 API: 同步原语 | [x] **已完成** | 2026-05-08 |
| 5. 高层 API: Stream I/O (Reader/Writer/Connection) | [x] **已完成** | 2026-05-08 |
| 6. 低层 API: EventLoop 核心 + Handle/TimerHandle | [x] **已完成** | 2026-05-08 |
| 7. 低层 API: Transport/Protocol 集成 | [x] **已完成** | 2026-05-08 |
| 8. Selector 抽象层 + 平台后端 | [x] **已完成** | 2026-05-08 |
| 9. Runners（Run, Runner） | [x] **已完成** | 2026-05-08 |
| 10. EventLoopPolicy + 全局便捷函数 | [x] **已完成** | 2026-05-08 |
| 11. TaskGroup 结构化并发 | [x] **已完成** | 2026-05-08 |
| 12. 低层 Socket I/O（SockRecv/SockSendall 等） | [ ] **待开始** | - |
| 13. Queue 补充（PutNowait/GetNowait/Priority/Lifo） | [ ] **待开始** | - |
| 14. TimeoutAt | [ ] **待开始** | - |
| 15. 子进程支持 | [ ] **待开始** | - |
| 16. 信号处理 + 执行器 | [ ] **待开始** | - |
| 17. SSL/TLS + Unix Domain Socket | [ ] **待开始** | - |
| 18. Windows IOCP 后端 | [ ] **待开始** | - |
| 19. 测试补全 | [ ] **进行中** | - |
| 20. 示例程序 | [ ] **待开始** | - |
| 21. 文档 | [ ] **待开始** | - |

**总体完成度估算：约 65%**（核心机制 + 同步原语 + Stream I/O + Transport/Protocol 已完成）
