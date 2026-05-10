bind() 的用户态签名是：

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

这里的 const struct sockaddr * 不是说底层只有一种地址结构，而是一个 通用地址入口。

真正地址类型由：

addr->sa_family

决定，例如：

AF_UNIX  → struct sockaddr_un
AF_INET  → struct sockaddr_in
AF_INET6 → struct sockaddr_in6

bind() 的语义就是把一个 socket “assigning a name”，也就是给 socket 绑定本地地址；不同 address family 的绑定规则不同。

1. 用户态为什么传 struct sockaddr *

因为 C 没有真正的多态，所以 socket API 用这种模式：

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

然后实际上传：

struct sockaddr_in addr;
bind(fd, (const struct sockaddr *)&addr, sizeof(addr));

或者：

struct sockaddr_un addr;
bind(fd, (const struct sockaddr *)&addr, sizeof(addr));

关键是所有 sockaddr 派生结构开头都有 family 字段：

struct sockaddr_in {
    sa_family_t    sin_family; // AF_INET
    in_port_t      sin_port;
    struct in_addr sin_addr;
    ...
};

struct sockaddr_un {
    sa_family_t sun_family;    // AF_UNIX
    char        sun_path[];
};

所以内核先看前几个字节里的 family，再按对应协议族解释后面的布局。

2. syscall 入口大致路径

用户调用：

bind(fd, addr, addrlen);

内核大致走：

__sys_bind(fd, user_sockaddr, addrlen)
    ↓
sockfd_lookup_light(fd)
    ↓
move_addr_to_kernel(user_sockaddr, addrlen, kernel_sockaddr)
    ↓
sock->ops->bind(sock, kernel_sockaddr, addrlen)

其中 sock->ops 是 struct proto_ops *，在创建 socket 时就由 address family 决定了。Linux 内核网络文档也区分了 struct socket 和 struct sock：前者接近用户态 BSD socket 抽象，后者是内核协议层 socket 表示。

也就是说，bind() 本身不关心你是 AF_UNIX 还是 AF_INET，它只做通用分发：

fd → struct file → struct socket → socket->ops->bind()
3. const struct sockaddr * 到内核后会复制

const 只约束用户态函数：bind() 不会修改用户传入的地址结构。

但 syscall 进入内核后，内核不能直接信任用户指针，所以会把用户空间地址复制到内核空间，例如概念上：

struct sockaddr_storage address;

copy_from_user(&address, user_addr, addrlen);

之后内核使用自己的 kernel buffer，不再直接解引用用户态指针。

所以这里有两层含义：

用户态:
    const struct sockaddr *addr
    表示 libc / syscall 不会修改用户的 addr

内核态:
    struct sockaddr_storage address
    从用户态 copy 进来之后再解析
4. socket() 时已经决定了 bind 分派函数

例如：

int fd = socket(AF_INET, SOCK_STREAM, 0);

创建时大致会得到：

struct socket
  ├─ type = SOCK_STREAM
  ├─ ops  = inet_stream_ops
  └─ sk   = struct sock / tcp_sock / inet_sock

所以之后：

bind(fd, ...)

会调用：

inet_bind()

而：

int fd = socket(AF_UNIX, SOCK_STREAM, 0);

则得到：

struct socket
  ├─ ops = unix_stream_ops
  └─ sk  = struct unix_sock

之后 bind() 调：

unix_bind()

不是 bind() 每次根据 addr->sa_family 做大 switch，而是 socket 创建时已经把 ops 装好了。

5. AF_INET 的 bind 做什么

用户态：

struct sockaddr_in addr{};
addr.sin_family = AF_INET;
addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
addr.sin_port = htons(8080);

bind(fd, (struct sockaddr *)&addr, sizeof(addr));

内核 inet_bind() 大致做：

1. 检查 addrlen 是否够 sizeof(sockaddr_in)
2. 检查 sin_family 是否是 AF_INET
3. 解析 IP 地址和 port
4. 检查地址是否合法
5. 检查端口权限，例如 <1024 需要 CAP_NET_BIND_SERVICE
6. 检查端口是否已被占用
7. 把 local address / local port 写入 inet_sock
8. 把 socket 加入 bind hash table

绑定之后，socket 的本地端就有了：

local_ip
local_port

例如：

127.0.0.1:8080
6. AF_UNIX 的 bind 做什么

用户态：

struct sockaddr_un addr{};
addr.sun_family = AF_UNIX;
strcpy(addr.sun_path, "/tmp/app.sock");

bind(fd, (struct sockaddr *)&addr, sizeof(addr));

内核 unix_bind() 大致做：

1. 检查 addrlen
2. 检查 sun_family 是否是 AF_UNIX
3. 判断是 pathname socket 还是 abstract socket
4. 如果是 pathname:
      通过 VFS 在文件系统中创建一个 socket inode
      类型是 S_IFSOCK
      路径类似 /tmp/app.sock
5. 把这个名字关联到 struct unix_sock
6. 标记 socket 已绑定

Linux unix(7) 里也强调，pathname Unix socket 的 addrlen 应按 offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1 这类方式传入，说明 sockaddr_un 的长度语义和 sockaddr_in 不一样。

对于 Linux abstract namespace：

addr.sun_path[0] = '\0';
memcpy(addr.sun_path + 1, "name", 4);

则不会在文件系统创建路径，而是在内核的 abstract Unix socket namespace 里登记。

7. 所以 AF_UNIX 是不是文件系统？

不是。

更准确是：

AF_UNIX 是本地 IPC socket 协议族；
pathname AF_UNIX socket 可以借用文件系统路径作为名字。

pathname bind 时，文件系统参与：

路径解析
权限检查
创建 S_IFSOCK inode
名字到 socket 的关联

但数据传输不走文件系统读写逻辑，而是走 Unix socket 子系统：

unix_sock → sk_receive_queue → sk_buff
8. 为什么 addrlen 很重要

因为 struct sockaddr * 本身无法告诉内核真实结构长度。

所以必须传：

bind(fd, (struct sockaddr *)&addr, sizeof(addr));

内核用 addrlen 判断：

AF_INET:
    至少要 sizeof(struct sockaddr_in)

AF_INET6:
    至少要 sizeof(struct sockaddr_in6)

AF_UNIX:
    长度决定 sun_path 中有效名字有多长

尤其 AF_UNIX：

pathname:
    sun_path 是字符串，通常包含 '\0'

abstract:
    sun_path[0] == '\0'
    后续字节可以包含任意 byte
    名字长度由 addrlen 决定，不靠 strlen()
9. 核心对象关系
AF_INET
fd
 ↓
struct file
 ↓
struct socket
 ↓ ops = inet_stream_ops / inet_dgram_ops
 ↓
struct sock
 ↓
struct inet_sock / tcp_sock
 ↓
local IP + local port + bind hash
AF_UNIX
fd
 ↓
struct file
 ↓
struct socket
 ↓ ops = unix_stream_ops / unix_dgram_ops
 ↓
struct sock
 ↓
struct unix_sock
 ↓
pathname / abstract name / peer relation
10. 最核心的一句话

bind() 的参数是：

const struct sockaddr *addr

是因为它是一个 通用、多态的地址容器入口。

底层真正实现是：

fd 找到 struct socket
struct socket 里已有 proto_ops
调用 socket->ops->bind()
具体由 AF_INET / AF_UNIX / AF_INET6 的 bind 实现解析 sockaddr

所以：

bind()
  不是“绑定一个 struct sockaddr”
  而是“把用户传入的一段地址内存复制进内核，然后交给该 socket 所属协议族解释”


先理清 Windows 模拟 socketpair 的典型流程

text
text
1. listener = socket(AF_INET, SOCK_STREAM, 0)
2. bind(listener, 127.0.0.1:0)          // OS 分配随机端口，比如 54321
3. listen(listener, 1)                    // listener 进入 LISTEN 状态
4. client = socket(AF_INET, SOCK_STREAM, 0)
5. connect(client, 127.0.0.1:54321)      // 发起三次握手
6. server = accept(listener, ...)         // 完成握手，拿到 server 端 socket
7. close(listener)                        // ← 你问的是这一步

为什么 accept() 之后可以、也应该关闭 listener

核心原因是：accept() 返回的 server 是一个全新的、独立的 socket，它与 client 之间已经建立了完整的 TCP 连接，不再依赖 listener。


三者的关系如下：


text
text
close 前：

  client ←────── TCP 连接 ──────→ server
                                         ↑
  listener ──── LISTEN 状态              │
  (fd=3)        (仍在端口 54321 上)       │
                                    accept() 返回的新 socket
                                    (fd=5，有自己独立的 fd)

close(listener) 后：

  client ←────── TCP 连接 ──────→ server   ← 完全不受影响

  (listener 已销毁，fd=3 已释放)

accept() 的本质是：从 listen 队列中取出一个已完成三次握手的连接，创建一个新的 socket 来代表这条连接的服务器端。这个新 socket 拥有：


自己独立的文件描述符
自己独立的发送/接收缓冲区
完整的 TCP 状态机（ESTABLISHED）

listener 的职责在 accept() 返回那一刻就结束了。


不关闭 listener 会出什么问题

1. 资源泄漏
listener 占用一个文件描述符。如果你高频调用这个模拟的 socketpair()（比如每次事件循环迭代都创建），listener fd 会不断累积。


2. 端口耗尽
listener 持续绑定一个端口。Windows 临时端口范围默认约 16,000 个（49152–65535）。不关闭 listener 会持续占用这些端口。


3. 安全隐患
listener 处于 LISTEN 状态，机器上的任何进程都可以 connect 过来：


text
text
// 恶意或意外的进程：
connect(any_socket, 127.0.0.1:54321);  // 成功！进入 accept 队列

你的 selector 在等待 server/client 上的事件时，listener 上也可能出现可读事件（有新连接到达），这会引入意外行为。


4. 对 select 的影响
回到你之前的 SelectSelector 代码——listener 如果不关闭，它会留在 entries_ 里，占一个 FD_SETSIZE 的名额，且可能触发无意义的可读事件。

