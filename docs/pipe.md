pipe 是 VFS/filesystem 风格的内核对象；socketpair(AF_UNIX, ...) 是 networking stack 风格的内核对象。

两者都占用两个 fdtable slot，也都经由 struct file * 进入内核，但 struct file 后面接的东西完全不同。

1. pipe() 的对象关系

简化后大概是：

task_struct
  └─ files_struct
      └─ fdtable
          ├─ fd[3] ──> struct file 读端
          └─ fd[4] ──> struct file 写端

struct file
  └─ private_data ──> struct pipe_inode_info
                         ├─ wait_queue_head_t wait
                         ├─ pipe_buffer ring
                         ├─ readers / writers
                         └─ mutex

pipe 的数据结构核心是：

pipe_inode_info + pipe_buffer ring

数据在 pipe buffer ring 里流动。

2. socketpair(AF_UNIX, SOCK_STREAM) 的对象关系

socketpair() 也会分配两个 fd slot：

task_struct
  └─ files_struct
      └─ fdtable
          ├─ fd[3] ──> struct file A
          └─ fd[4] ──> struct file B

但每个 struct file 后面不是 pipe_inode_info，而是 socket 对象：

struct file
  └─ private_data ──> struct socket
                         ├─ struct sock *sk
                         ├─ const struct proto_ops *ops
                         └─ socket state

然后：

struct socket A
  └─ sk ──> struct sock A
              ├─ sk_receive_queue
              ├─ sk_write_queue
              ├─ sk_sleep / wait queue
              ├─ sk_err
              ├─ sk_shutdown
              └─ protocol-private data

struct socket B
  └─ sk ──> struct sock B
              ├─ sk_receive_queue
              ├─ sk_write_queue
              ├─ sk_sleep / wait queue
              └─ ...

对于 Unix domain socket，还有一层：

struct unix_sock A
  ├─ struct sock sk
  └─ peer ──> unix_sock B

struct unix_sock B
  ├─ struct sock sk
  └─ peer ──> unix_sock A

所以 socketpair() 不是“两端共享同一个 ring buffer”，而是：

两端各有一个 socket / sock；A 写入时，把 skb 挂到 B 的 receive queue；B 写入时，把 skb 挂到 A 的 receive queue。

Linux networking 文档也把 socket 侧核心结构概括为 struct socket、struct sock、struct sk_buff 三类对象；struct socket 更接近用户态 BSD socket 抽象，struct sock 是内核网络层表示，数据包则由 sk_buff 表示。

3. socketpair 的读写路径

以：

int sv[2];
socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
write(sv[0], "x", 1);
read(sv[1], buf, 1);

为例。

写端 sv[0]

路径大致是：

write(fd)
  → struct file->f_op->write_iter
  → sock_write_iter()
  → sock_sendmsg()
  → unix_stream_sendmsg()
  → 找到 peer socket
  → 分配 / 构造 sk_buff
  → skb_queue_tail(peer->sk_receive_queue, skb)
  → wake_up(peer->sk_sleep)

也就是说：

sv[0] write
    ↓
struct sock A
    ↓
peer = struct sock B
    ↓
B.sk_receive_queue.push(skb)
    ↓
唤醒等待 B 可读的任务
读端 sv[1]

路径大致是：

read(fd)
  → struct file->f_op->read_iter
  → sock_read_iter()
  → sock_recvmsg()
  → unix_stream_recvmsg()
  → 从当前 socket 的 sk_receive_queue 取 skb
  → copy_to_user()

如果 sk_receive_queue 为空，就根据阻塞/非阻塞状态决定：

非阻塞: -EAGAIN
阻塞: 当前 task 挂到 socket wait queue，调度出去

AF_UNIX 的 poll 路径里会检查 sk_receive_queue 是否为空；源码里的 unix_poll() 逻辑就是：如果 receive queue 非空，就返回 EPOLLIN | EPOLLRDNORM。

4. 和 pipe 最大的结构差异
pipe
读 fd ─┐
       ├──> 同一个 pipe_inode_info
写 fd ─┘

pipe 是一个共享 pipe object：

pipe_inode_info
  ├─ ring buffer
  ├─ wait queue
  ├─ readers
  └─ writers

读端和写端围绕同一个 pipe_inode_info 协作。

socketpair
fd A → file A → socket A → sock A ←peer→ sock B ← socket B ← file B ← fd B

socketpair 是两个 socket 互为 peer：

sock A.receive_queue  接收 B 写来的数据
sock B.receive_queue  接收 A 写来的数据

数据单元通常是 sk_buff，挂在对端的 sk_receive_queue 上。

5. 等待队列也不同

pipe 通常围绕 pipe_inode_info 上的等待队列来等待：

pipe_inode_info.wait

socket 则围绕 socket/sock 的等待机制：

struct socket / struct sock
  └─ wait queue via sk_sleep / socket waitqueue

poll/select/epoll 调 socket 的 poll 回调时，会把当前 poll table 注册到 socket 的 wait queue；之后 peer 写入数据时，内核通过 socket wakeup 机制唤醒等待者。

6. 数据表示不同

pipe 的数据：

pipe_buffer
  ├─ struct page *page
  ├─ offset
  ├─ len
  └─ pipe_buf_operations

socketpair 的数据：

sk_buff
  ├─ data pointer
  ├─ len
  ├─ control metadata
  ├─ ownership / accounting
  └─ queue linkage

所以 pipe 更像：

page-backed byte stream ring

Unix socket 更像：

skb-backed message/stream queue

即使 SOCK_STREAM 对用户呈现字节流，内核内部仍然用 skb queue 承载数据。

7. 一个很重要的语义差别：全双工 vs 半双工

pipe() 是单向的：

write_fd → read_fd

如果想双向通信，需要两个 pipe。

socketpair() 天然全双工：

sv[0] → sv[1]
sv[1] → sv[0]

所以它内部必须是两个接收队列，而不是一个共享 ring。

8. 总结成一句话

pipe()：

两个 fd 指向同一个 pipe_inode_info；
数据在 pipe_buffer ring 中；
读写双方共享一个 pipe wait queue / 状态对象。

socketpair(AF_UNIX, SOCK_STREAM)：

两个 fd 各自指向一个 struct socket；
每个 socket 有自己的 struct sock 和 receive queue；
写一端时，数据被封装成 sk_buff，挂到 peer 的 sk_receive_queue；
读一端时，从自己的 sk_receive_queue 取 skb。