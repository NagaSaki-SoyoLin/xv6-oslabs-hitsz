#include "kernel/types.h"
#include "user/user.h"

#include "ai_student_impl.h"

int
ai_student_kv_begin(struct ai_session *session)
{
  // TODO(LAB3-AI，任务四)：初始化当前 worker 的 KV 持久化状态。
  // 可采用每请求文件，也可采用每 worker 顺序流，但不得保留内存副本绕过恢复。
  (void)session;
  return -1;
}

int
ai_student_kv_begin_write(struct ai_session *session)
{
  // TODO(LAB3-AI，任务四)：开始 KV 写入阶段并准备所需文件。
  (void)session;
  return -1;
}

int
ai_student_kv_store(struct ai_session *session, int request,
                    struct kv_entry *kv, struct ai_io *io)
{
  // TODO(LAB3-AI，任务四)：把当前请求的完整 KV cache 写入 xv6 文件系统。
  // 必须循环处理短写；只有 AI_KV_FILE_BYTES 字节全部成功后才更新统计。
  // aiinfer 随后会立即释放原页面，因此不能依赖 kv 指针中的残留数据。
  (void)session;
  (void)request;
  (void)kv;
  (void)io;
  return -1;
}

int
ai_student_kv_end_write(struct ai_session *session)
{
  // TODO(LAB3-AI，任务四)：结束写阶段，确保文件状态完整并关闭写描述符。
  (void)session;
  return -1;
}

int
ai_student_kv_begin_read(struct ai_session *session)
{
  // TODO(LAB3-AI，任务四)：重新打开持久化文件，从文件起点开始恢复。
  (void)session;
  return -1;
}

int
ai_student_kv_restore(struct ai_session *session, int request,
                      struct kv_entry *kv, struct ai_io *io)
{
  // TODO(LAB3-AI，任务四)：精确恢复当前请求的完整 KV cache。
  // 目标页已被覆盖，必须循环处理短读；全部成功后再更新读取字节数。
  // 单项验证：先运行 modelprep，再运行 aikvtest。
  (void)session;
  (void)request;
  (void)kv;
  (void)io;
  return -1;
}

void
ai_student_kv_end(struct ai_session *session)
{
  // TODO(LAB3-AI，任务四)：关闭 KV 文件并删除当前 worker 的临时持久化文件。
  (void)session;
}
