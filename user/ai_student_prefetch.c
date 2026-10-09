#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#include "ai_student_impl.h"

static int
read_exact(int fd, void *buffer, int length)
{
  char *p = buffer;
  int n;

  // read() 返回的是本次实际读到的字节数，不能假设一次调用就拿到
  // 整条记录。循环结束才表示 length 字节已经完整交付给调用者。
  while(length > 0) {
    n = read(fd, p, length);
    if(n <= 0)
      return -1;
    p += n;
    length -= n;
  }
  return 0;
}

static int
write_exact(int fd, void *buffer, int length)
{
  char *p = buffer;
  int n;

  // 与 read_exact 对称地处理短写；KV 文件只有完整写入后才允许计入
  // kv_write_bytes，失败时还要由上层删除不完整文件。
  while(length > 0) {
    n = write(fd, p, length);
    if(n <= 0)
      return -1;
    p += n;
    length -= n;
  }
  return 0;
}

// 父进程一侧的预取状态（fork 后每进程独立一份副本）
static int pf_active; // begin 是否成功建立过（end 据此判断）
static int pf_pid; // 学徒 pid；正常路径下 > 0
static int pf_req_write; // 派活管道写端（父持有）
static int pf_data_read; // 交货管道读端（父持有）
static int pf_pending; // 在途编号；-1 = 无
static int pf_ready; // 到货编号；-1 = 无
static char pf_slot[AI_KV_FILE_BYTES]; // 到货记录的接收槽（2048 字节）

// 学徒（辅助进程）: 按父进程发来的顺序, 从自己的只读 fd 读完整, 记录并经管道回传, 只在子进程中运行, 永不返回
static void
helper_loop(struct ai_session *session, int req_read, int data_write)
{
  char name[AI_KV_NAME_LEN];
  static char scratch[AI_KV_FILE_BYTES]; // static：避免撑爆单页用户栈
  int fd;
  int request;
  int expected = 0; // 请求严格按 0,1,2... 顺序到达

  // 必须自行打开只读 fd：fork 继承的 fd 与父进程共享文件偏移，
  // 父子交替读会把偏移彻底推乱。谁开谁用，所有权清晰。
  ai_student_kv_name(name, session->worker);
  fd = open(name, O_RDONLY);
  if(fd < 0)
    exit(1);

  for(;;) {
    if(read_exact(req_read, &request, sizeof(request)) < 0)
      break; // 派活端关闭（EOF）也视为停止信号
    if(request < 0)
      break; // 显式停止编号
    if(request != expected)
      exit(1); // 错序：不回半条消息，父进程自会发现
    // 顺序读自己的 fd：第 expected 条的偏移恰好自动对齐，expected * 2048，与顺序文件设计严丝合缝。
    if(read_exact(fd, scratch, AI_KV_FILE_BYTES) < 0)
      exit(1);
    // 交货：编号先行（父进程核对"回复==它在等的"），再交 2048 字节。
    if(write_exact(data_write, &request, sizeof(request)) < 0 ||
       write_exact(data_write, scratch, AI_KV_FILE_BYTES) < 0)
      exit(1); // 交货端已关（父进程先走了）：退出
    expected++;
  }

  close(fd);
  close(req_read);
  close(data_write);
  exit(0);
}

int
ai_student_prefetch_begin(struct ai_session *session)
{
  // TODO(LAB3-AI，选做)：建立固定小窗口的 KV 预取状态。
  // 推荐使用轻量辅助进程提前读取下一条记录，让数据进入共享 buffer cache。
  // 不允许扩大 NBUF，也不能跳过附加题二要求的真实恢复。
  int req_pipe[2];
  int data_pipe[2];
  int pid;

  if(pf_active) // 防御：不该重复 begin，先收拾残局
    ai_student_prefetch_end(session);

  if(pipe(req_pipe) < 0)
    return -1;
  if(pipe(data_pipe) < 0) {
    close(req_pipe[0]); // 第一条管道两端
    close(req_pipe[1]);
    return -1;
  }
  pid = fork();
  if(pid < 0) {
    close(req_pipe[0]); // 四端全关，且无子进程可 wait
    close(req_pipe[1]);
    close(data_pipe[0]);
    close(data_pipe[1]);
    return -1;
  }
  if(pid == 0) {
    // 学徒侧：只留派活读端 + 交货写端。
    // 不立刻关闭多余端的后果：EOF 语义失灵。例如父进程想靠
    // 交货管道 EOF 发现学徒死亡，但自己手里还捏着交货写端，
    // 内核认为"还有写者"，父进程将永远阻塞。
    close(req_pipe[1]);
    close(data_pipe[0]);
    helper_loop(session, req_pipe[0], data_pipe[1]);
  }

  // 工人侧：只留派活写端 + 交货读端，立刻关闭多余两端。
  close(req_pipe[0]);
  close(data_pipe[1]);

  pf_pid = pid;
  pf_req_write = req_pipe[1];
  pf_data_read = data_pipe[0];
  pf_pending = -1;
  pf_ready = -1;
  pf_active = 1;
  return 0;
}

int
ai_student_prefetch_next(struct ai_session *session, int request)
{
  // TODO(LAB3-AI，选做)：非阻塞地请求预取给定 request 的 KV 记录。
  if(!pf_active)
    return -1;
  if(request < 0 || request >= session->requests)
    return -1; // 越界单子
  if(pf_pending != -1 || pf_ready != -1)
    return -1; // 只有 EMPTY 才能派：PENDING 时再派会覆盖，
  // READY 时再派会顶掉尚未消费的货
  if(write_exact(pf_req_write, &request, sizeof(request)) < 0)
    return -1; // 编号没写完整，不算派出
  pf_pending = request; // 完整写出后才进入 PENDING
  return 0;
}

int
ai_student_prefetch_wait(struct ai_session *session, int request)
{
  // TODO(LAB3-AI，选做)：等待对应预取完成，且不得改变父 worker 的文件偏移。
  int reply;

  (void)session;

  if(!pf_active)
    return -1;
  if(pf_pending != request) // 必须 PENDING(request)，EMPTY/READY/错号都拒
    return -1;
  if(read_exact(pf_data_read, &reply, sizeof(reply)) < 0)
    return -1;
  if(reply != request) // 迟到/错序回复：不交付、不记账
    return -1;
  if(read_exact(pf_data_read, pf_slot, AI_KV_FILE_BYTES) < 0)
    return -1; // 半条记录不算数
  pf_pending = -1;
  pf_ready = request; // 编号与数据都完整收到，才进 READY
  return 0;
}

int
ai_student_prefetch_restore(struct ai_session *session, int request,
                            struct kv_entry *kv, struct ai_io *io)
{
  // TODO(LAB3-AI，选做)：把已由辅助进程从磁盘读取的完整记录交给父 worker。
  // 这次恢复仍必须对应一次真实文件读取，并且只在完整交付后累计 KV 字节数。
  (void)session;

  if(!pf_active)
    return -1;
  if(pf_ready != request) // 必须 READY(request)；EMPTY 时无货可交
    return -1;
  memcpy(kv, pf_slot, AI_KV_FILE_BYTES);
  io->kv_read_bytes += AI_KV_FILE_BYTES; // 完整交付后才记账
  pf_ready = -1; // 回到 EMPTY，可以派下一单
  return 0;
}

void
ai_student_prefetch_end(struct ai_session *session)
{
  // TODO(LAB3-AI，选做)：关闭管道、等待辅助进程并释放全部预取资源。
  int stop = -1;

  (void)session;

  if(!pf_active) // 从未建立或已清理：直接返回
    return;

  // 停止信号尽力发（4 字节 < 管道缓冲，单次 write 足够；失败无妨——
  // 下一步关闭派活写端后，学徒会从 read 得到 EOF 而退出）。
  if(pf_req_write != -1) {
    write(pf_req_write, &stop, sizeof(stop));
    close(pf_req_write);
    pf_req_write = -1;
  }
  // 关交货读端：若学徒正阻塞在写管道上，读端关闭会唤醒它并使
  // write 返回 -1，学徒随之退出——不会死锁。
  if(pf_data_read != -1) {
    close(pf_data_read);
    pf_data_read = -1;
  }
  // 回收学徒，防止僵尸。wait 阻塞到学徒退出为止；此后才轮到
  // 适配层调用 kv_end 去 unlink 文件——学徒的 fd 已随 exit 关闭。
  if(pf_pid > 0) {
    wait(0);
    pf_pid = -1;
  }
  pf_pending = -1;
  pf_ready = -1;
  pf_active = 0;
}
