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

int
ai_student_kv_begin(struct ai_session *session)
{
  // TODO(LAB3-AI，附加题二)：初始化当前 worker 的 KV 持久化状态。
  // 可采用每请求文件，也可采用每 worker 顺序流，但不得保留内存副本绕过恢复。
  session->state[1] = 0; // 下一个要 store 的 request 编号
  session->state[2] = 0; // 下一个要 restore 的 request 编号
  session->state[3] = -1; // 当前打开的文件描述符
  return 0;
}

int
ai_student_kv_begin_write(struct ai_session *session)
{
  // TODO(LAB3-AI，附加题二)：开始 KV 写入阶段并准备所需文件。
  char name[AI_KV_NAME_LEN];
  int fd;

  ai_student_kv_name(name, session->worker); // 生成 worker 标识文件
  unlink(name); // 删除旧文件, 防止上轮残留
  fd = open(name, O_CREATE | O_WRONLY);
  if(fd < 0)
    return -1;

  session->state[1] = 0; // 下一个要 store 的 request 编号
  session->state[3] = fd; // 保存当前文件描述符
  return 0;
}

int
ai_student_kv_store(struct ai_session *session, int request,
                    struct kv_entry *kv, struct ai_io *io)
{
  // TODO(LAB3-AI，附加题二)：把当前请求的完整 KV cache 写入 xv6 文件系统。
  // request 必须是 [0, session->requests) 内预期的下一条；非法序号返回 -1，
  // 且不得改变文件位置、下一请求序号、KV 内容或 I/O 统计，后续合法调用仍须可用。
  // 必须循环处理短写；只有 AI_KV_FILE_BYTES 字节全部成功后才更新统计。
  // aiinfer 随后会立即释放原页面，因此不能依赖 kv 指针中的残留数据。
  if (request < 0 || request >= session->requests
    || request != session->state[1])
    return -1; // 非法序号检测
  if(write_exact(session->state[3], kv, AI_KV_FILE_BYTES) < 0) {
    return -1;
  }
  io->kv_write_bytes += AI_KV_FILE_BYTES;
  session->state[1]++; // 下一个要 store 的 request 编号
  return 0;
}

int
ai_student_kv_end_write(struct ai_session *session)
{
  // TODO(LAB3-AI，附加题二)：结束写阶段，确保文件状态完整并关闭写描述符。
  if (session->state[3] != -1) {
    close(session->state[3]);
    session->state[3] = -1;
  }
  return 0;
}

int
ai_student_kv_begin_read(struct ai_session *session)
{
  // TODO(LAB3-AI，附加题二)：重新打开持久化文件，从文件起点开始恢复。
  char name[AI_KV_NAME_LEN];
  int fd;

  ai_student_kv_name(name, session->worker); // 生成 worker 标识文件
  fd = open(name, O_RDONLY);
  if(fd < 0)
    return -1;

  session->state[2] = 0; // 下一个要 restore 的 request 编号
  session->state[3] = fd; // 保存当前文件描述符
  return 0;
}

int
ai_student_kv_restore(struct ai_session *session, int request,
                      struct kv_entry *kv, struct ai_io *io)
{
  // TODO(LAB3-AI，附加题二)：精确恢复当前请求的完整 KV cache。
  // request 必须是 [0, session->requests) 内预期的下一条；非法序号返回 -1，
  // 且不得改变文件位置、下一请求序号、目标 KV 内容或 I/O 统计，后续合法调用仍须可用。
  // 目标页已被覆盖，必须循环处理短读；全部成功后再更新读取字节数。
  // 单项验证：先运行 modelprep，再运行 aikvtest。
  if (request < 0 || request >= session->requests
    || request != session->state[2])
    return -1; // 非法序号检测
  if(read_exact(session->state[3], kv, AI_KV_FILE_BYTES) < 0) {
    return -1;
  }
  io->kv_read_bytes += AI_KV_FILE_BYTES;
  session->state[2]++; // 下一个要 restore 的 request 编号
  return 0;
}

void
ai_student_kv_end(struct ai_session *session)
{
  // TODO(LAB3-AI，附加题二)：关闭 KV 文件并删除当前 worker 的临时持久化文件。
  char name[AI_KV_NAME_LEN];

  // 重置 state 状态
  session->state[1] = 0;
  session->state[2] = 0;
  if (session->state[3] != -1) {
    close(session->state[3]);
    session->state[3] = -1;
  }

  ai_student_kv_name(name, session->worker);
  unlink(name); // 删除旧文件, 防止上轮残留
}
