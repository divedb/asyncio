# MEMORY.md - Long-term Memory

## 项目背景
- 用户 zlh 是 C++ 开发者，正在并行推进多个项目：核心是开发 chibicpp C++20 编译器（位于 /Users/zlh/Documents/git/chibicpp/）。同时正在用 C++ 完整复刻 Python asyncio（位于 /Users/zlh/Documents/git/asyncio/）。
- 偏好中文进行技术交流，沟通风格直接坦率，技术性强，熟练使用编译器/后端术语。
- 日常使用 WorkBuddy 桌面客户端（基于 Electron）进行 AI 辅助编程。

## asyncio-cpp 项目（/Users/zlh/Documents/git/asyncio/）

### 项目概述
用 C++20 完整复刻 Python asyncio 的 API 和语义，使用 C++20 原生协程（`co_await`/`co_return`/`co_yield`）作为异步操作的核心机制。

### 架构设计亮点
- **零外部依赖** — 仅使用 C++20 标准库
- **跨平台** — 自动选择最优 I/O 后端 (epoll/kqueue/IOCP/select)
- **协作式取消** — `Task::Cancel()` 注入 `AsyncCancelledError`
- **结构化并发** — 支持 `Cancelling()`/`Uncancel()`（Python 3.11+ 语义）
- **RAII 模式** — `LockGuard`, `TimeoutScope` 等作用域管理

### 已实现模块
1. **异常类型体系** (`error.h`) — AsyncError, AsyncCancelledError, InvalidStateError, AsyncTimeoutError, BrokenBarrierError, QueueShutDownError, IncompleteReadError, LimitOverrunError
2. **Future/Task** (`future.h`, `task.h`) — 状态机 + C++20 协程支持
3. **同步原语** (`sync/*.h`) — AsyncEvent, AsyncLock, AsyncSemaphore, AsyncCondition, AsyncBarrier, AsyncQueue
4. **高层 API** — Gather, Shield, Wait, WaitFor, TimeoutScope, Sleep, Yield
5. **事件循环** (`event_loop.h`, `event_loop.cc`) — RunForever/RunOnce/Stop, CallSoon/Later/At, I/O 注册
6. **Selector 抽象层** — EpollSelector, KqueueSelector, SelectSelector, 自动后端选择
7. **Stream I/O** — StreamReader, StreamWriter, OpenConnection, StartServer, Server
8. **Transport/Protocol** — SocketTransport, ProtocolBase, StreamProtocol
9. **Runners** (`runner.h`) — `asyncio::Run()`, `Runner` 类

### 重要 API 变更记录
- **2026-05-08**: `asyncio::Run()` 和 `Runner::Run()` 现在接收**工厂函数**而非 `Task<T>` 对象，例如：`Run(MyCoroutine)` 而非 `Run(MyCoroutine())`。这是因为需要确保 EventLoop 在协程启动前就已设置为 current。

### 近期动态
- 2026-05-08: 完成 Runners（`asyncio::Run()` + `Runner` 类），对标 Python `asyncio.run()` 和 `asyncio.Runner`
- 2026-05-08: 完成 TODO.md 全面更新，对标 Python asyncio 列出所有缺失功能
- 2026-05-08: 同步原语（AsyncEvent/Lock/Semaphore/Condition/Barrier/Queue）全部完成
- 2026-05-08: Selector 抽象层（Epoll/Kqueue/Select）全部完成
- 2026-05-08: Stream I/O（Reader/Writer/Connection）全部完成
- 2026-05-08: EventLoop 核心（RunForever/RunOnce, Handle/TimerHandle）全部完成
- 2026-05-08: Transport/Protocol 集成全部完成

### 待完成（按优先级）
1. **EventLoopPolicy** — 全局策略 + 便捷函数（GetEventLoop 等）
2. **TaskGroup** — 结构化并发（Python 3.11+）
3. **低层 Socket I/O** — SockRecv/SockSendall/SockConnect/SockAccept, GetAddrInfo
4. **Queue 补充** — PutNowait/GetNowait, PriorityQueue, LifoQueue
5. **TimeoutAt** — 绝对时间超时
6. **子进程** — Subprocess, Process
7. **信号 + 执行器** — AddSignalHandler, RunInExecutor
8. **SSL/TLS + Unix Domain Socket**
9. **Windows IOCP 后端**
10. **测试补全 + 示例程序 + 文档**

### 总体完成度
约 **70%**（核心机制 + 同步原语 + Stream I/O + Transport/Protocol + Runners 已完成）
