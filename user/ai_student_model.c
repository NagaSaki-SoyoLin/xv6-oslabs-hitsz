#include "kernel/types.h"
#include "user/user.h"

#include "ai_student_impl.h"

int
ai_student_model_begin(struct ai_session *session)
{
  // TODO(LAB3-AI，任务三)：初始化当前 worker 的模型加载状态。
  // 可以在这里申请 worker 私有缓存；不得修改全局模型文件或扩大 NBUF。
  // 单项验证：先运行 modelprep，再运行 aimodeltest。
  (void)session;
  return -1;
}

int
ai_student_model_load(struct ai_session *session, uint family, uint shard,
                      char *block, struct ai_io *io)
{
  // TODO(LAB3-AI，任务三)：加载一个模型块或 embedding shard。
  // 首次数据必须来自 xv6 文件系统，成功时 block 中必须恰好有
  // AI_SHARD_BYTES 字节；只有真实读取成功后才能更新 io 字节数。
  // 建议先读懂 ai_baseline.c 的经典 open-read-close 数据流，再考虑缓存。
  (void)session;
  (void)family;
  (void)shard;
  (void)block;
  (void)io;
  return -1;
}

void
ai_student_model_end(struct ai_session *session)
{
  // TODO(LAB3-AI，任务三)：释放缓存并关闭尚未关闭的模型文件描述符。
  // 失败路径和正常路径都必须能够安全调用本函数，资源只能释放一次。
  (void)session;
}
