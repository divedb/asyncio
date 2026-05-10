一、FD_CLOEXEC 是什么

当一个进程执行：

execve(...)

时，默认情况下：

所有打开的 file descriptor 都会被继承到新程序中。

如果某个 fd 设置了：

FD_CLOEXEC

那么在 execve() 时内核会自动关闭它（close-on-exec）。

二、为什么要设置 FD_CLOEXEC

例如你的事件循环创建了 wakeup pipe：

fd 10 = read end
fd 11 = write end

如果程序之后启动子进程：

fork();
execve(...);

而没有设置 FD_CLOEXEC，子进程也会持有：

fd 10
fd 11

这会导致：

文件描述符泄漏
pipe/socket 引用计数增加
父进程关闭后 EOF 不出现
难以调试的资源问题

所以系统库通常都会设置 FD_CLOEXEC。

三、非原子方式（旧方式）
int fd = socket(...);      // 先创建
fcntl(fd, F_SETFD, FD_CLOEXEC); // 再设置

问题在于这两步之间有时间窗口。

四、竞态条件

假设两个线程：

线程 A
1. socketpair()
2. （还没来得及 fcntl(FD_CLOEXEC)）
线程 B
3. fork()
4. execve()
结果

子进程继承了这个新 fd。

然后线程 A 才执行：

5. fcntl(FD_CLOEXEC)

但已经太晚了。

五、原子方式（Linux 2.6.27+）

Linux 支持：

socketpair(AF_UNIX,
           SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
           0,
           sv);

内核在创建 fd 的同时设置：

O_NONBLOCK
FD_CLOEXEC

于是：

fd 一诞生就带有 CLOEXEC

不存在任何时间窗口。

六、为什么 macOS/BSD 需要 fallback

macOS 和部分 BSD 的 socketpair() 不接受：

SOCK_NONBLOCK
SOCK_CLOEXEC

因此只能：

socketpair(...)
fcntl(... FD_CLOEXEC)
fcntl(... O_NONBLOCK)
七、什么时候是安全的

如果代码运行在：

单线程初始化阶段
还不会调用 fork()
没有其他线程并发创建子进程

那么这段时间不会发生：

socketpair() → fork+exec → fcntl()

因此可以认为是安全的。

八、图示
原子方式
kernel:
  create fd
  set CLOEXEC
  return fd

时间窗口：

0
非原子方式
user:
  socketpair()
     ↑
     | <--- race window --->
     ↓
  fcntl(FD_CLOEXEC)
九、你的注释含义
/// Linux 2.6.27+ accepts SOCK_NONBLOCK | SOCK_CLOEXEC ...

Linux 可以一次性设置这些属性。

/// This avoids the classic race where another thread could fork+exec
/// before FD_CLOEXEC is applied with fcntl().

避免“另一个线程在你设置 CLOEXEC 之前创建子进程”的竞态。

/// macOS / BSD ... fall back to separate fcntl() calls.

这些系统不支持原子设置，只能后续调用 fcntl()。

/// safe as long as no concurrent fork+exec can happen ...

只要这段时间没有其他线程执行 fork()+exec()，就没有风险。