#include "kernel/types.h"
#include "user/user.h"

#include "ai_student_impl.h"

int
ai_student_prefetch_begin(struct ai_session *session)
{
  // TODO(LAB3-AI，选做)：建立固定小窗口的 KV 预取状态。
  // 推荐使用轻量辅助进程提前读取下一条记录，让数据进入共享 buffer cache。
  // 不允许扩大 NBUF，也不能跳过任务四要求的真实恢复。
  (void)session;
  return -1;
}

int
ai_student_prefetch_next(struct ai_session *session, int request)
{
  // TODO(LAB3-AI，选做)：非阻塞地请求预取给定 request 的 KV 记录。
  (void)session;
  (void)request;
  return -1;
}

int
ai_student_prefetch_wait(struct ai_session *session, int request)
{
  // TODO(LAB3-AI，选做)：等待对应预取完成，且不得改变父 worker 的文件偏移。
  (void)session;
  (void)request;
  return -1;
}

int
ai_student_prefetch_restore(struct ai_session *session, int request,
                            struct kv_entry *kv, struct ai_io *io)
{
  // TODO(LAB3-AI，选做)：把已由辅助进程从磁盘读取的完整记录交给父 worker。
  // 这次恢复仍必须对应一次真实文件读取，并且只在完整交付后累计 KV 字节数。
  (void)session;
  (void)request;
  (void)kv;
  (void)io;
  return -1;
}

void
ai_student_prefetch_end(struct ai_session *session)
{
  // TODO(LAB3-AI，选做)：关闭管道、等待辅助进程并释放全部预取资源。
  (void)session;
}
