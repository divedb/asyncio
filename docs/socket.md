你贴出的这段 struct sock 定义，是 Linux 网络协议栈里最核心的数据结构之一。

可以把它理解成：

一个 TCP/UDP socket 在内核中的“总控制块（control block）”。

也就是说，用户态的：

int fd = socket(AF_INET, SOCK_STREAM, 0);

最终在内核里会对应一个 struct sock（更准确地说，TCP 通常是 struct tcp_sock，其最底层基类是 struct sock）。

一、整体作用

struct sock 保存了 socket 的所有核心状态：

本地/远程 IP 和端口
TCP 状态（LISTEN、ESTABLISHED 等）
收发缓冲区
等待队列
错误状态
定时器
回调函数
锁
用户身份
优先级

所以：

struct sock 就是 Linux 内核对“一个网络连接”的完整描述。

二、最重要的第一行
struct sock_common __sk_common;

这是最核心的“公共基类”。

为什么要单独抽出来

因为以下结构都要共享这些字段：

struct sock
struct inet_sock
struct tcp_sock
struct udp_sock
struct inet_timewait_sock

所以 Linux 把最通用的信息放进 sock_common。

三、宏别名的作用

你看到大量：

#define sk_state      __sk_common.skc_state
#define sk_num        __sk_common.skc_num
#define sk_dport      __sk_common.skc_dport
#define sk_rcv_saddr  __sk_common.skc_rcv_saddr
#define sk_daddr      __sk_common.skc_daddr

这些并不是新字段，而是“快捷访问”。

例如
sk->sk_state

实际等价于：

sk->__sk_common.skc_state
四、最关键的几个字段
1. sk_state
#define sk_state __sk_common.skc_state

表示 TCP 当前状态：

TCP_CLOSE
TCP_LISTEN
TCP_SYN_SENT
TCP_ESTABLISHED
TCP_FIN_WAIT1
TCP_TIME_WAIT
2. sk_num

本地端口号。

例如：

bind(..., 8888);

之后：

sk->sk_num = 8888;
3. sk_dport

远端端口号（网络字节序）。

4. sk_rcv_saddr

本地 IP 地址。

5. sk_daddr

远端 IP 地址。

五、接收队列
struct sk_buff_head sk_receive_queue;

这是 socket 的已接收数据队列。

工作流程

网卡收到数据包：

NIC → TCP → sk_receive_queue

用户调用：

recv(fd, ...)

内核就从该队列中取数据。

六、发送队列
struct sk_buff_head sk_write_queue;

保存待发送或等待确认的数据。

七、回调函数
void (*sk_data_ready)(struct sock *sk);

当有新数据到达时，协议栈调用这个函数。

默认行为

该回调会唤醒：

select()
poll()
epoll
阻塞的 recv()

所以 epoll_wait() 能返回，本质上就是这里的回调触发了等待队列。

八、等待队列
struct socket_wq *sk_wq;

其中包含：

等待 recv()
等待 send()
等待 epoll

的任务队列。

九、错误状态
int sk_err;
int sk_err_soft;

保存 socket 的错误，例如：

ECONNRESET
ETIMEDOUT
EPIPE
十、反向指针
struct socket *sk_socket;

这使得协议层能够回到 BSD socket 层。

十一、锁
socket_lock_t sk_lock;

保护整个 socket 状态，避免并发访问问题。

十二、定时器
struct timer_list tcp_retransmit_timer;

用于：

重传
keepalive
超时检测
十三、关闭状态
u8 sk_shutdown;

这是你前面问到 shutdown() 时最关键的字段。

典型值
RCV_SHUTDOWN = 1
SEND_SHUTDOWN = 2

例如：

shutdown(fd, SHUT_WR);

之后：

sk->sk_shutdown |= SEND_SHUTDOWN;

表示：

“这个 socket 以后不能再发送数据。”

十四、缓冲区大小
int sk_rcvbuf;
int sk_sndbuf;

对应：

SO_RCVBUF
SO_SNDBUF
十五、用户身份
kuid_t sk_uid;

记录创建该 socket 的用户 ID。

十六、结构体分组（cacheline）

你看到的：

__cacheline_group_begin(...)

是为了 CPU cache 优化：

高频读写字段放在一起
减少 cache miss
提升多核性能
十七、继承关系
struct sock
   ↑
struct inet_sock
   ↑
struct inet_connection_sock
   ↑
struct tcp_sock

因此：

struct sock 提供通用字段
struct tcp_sock 增加 TCP 专属字段（拥塞窗口、序号等）
十八、最重要的五个字段

如果只记住最关键的内容：

sk_state
sk_receive_queue
sk_write_queue
sk_data_ready
sk_shutdown