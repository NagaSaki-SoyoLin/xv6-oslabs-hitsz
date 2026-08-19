#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#include "ai_path.h"

// baseline 是一条“正确但低效”的经典链路，专门作为 student 的语义和
// 性能对照。它不缓存模型分片，每次 KV 请求都使用独立文件；学生可以
// 优化重复工作，但不能改变这里体现的文件、I/O、checksum 和输出语义。

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

static int
baseline_begin(struct ai_session *session, int worker, int requests)
{
  // session 只属于当前 worker。baseline 不需要额外缓存状态，因此只
  // 保存 worker 编号和请求数，后续每次调用独立打开并关闭文件。
  memset(session, 0, sizeof(*session));
  session->worker = worker;
  session->requests = requests;
  return 0;
}

static int
baseline_load(struct ai_session *session, uint family, uint shard,
              char *block, struct ai_io *io)
{
  char name[AI_SHARD_NAME_LEN];
  int fd;
  int offset;

  (void)session;
  // 先拒绝越界编号，再根据 family 生成 mXX/eXX 文件名。baseline 每次
  // 都执行 open -> read_exact -> close，故意保留重复 I/O 作为优化基线。
  if((family == AI_MODEL_FAMILY && shard >= AI_MODEL_SHARDS) ||
     (family == AI_EMBED_FAMILY && shard >= AI_EMBED_SHARDS))
    return -1;

  ai_shard_name(name, family, shard);
  fd = open(name, O_RDONLY);
  if(fd < 0)
    return -1;
  if(read_exact(fd, block, AI_SHARD_BYTES) < 0) {
    close(fd);
    return -1;
  }
  close(fd);

  // 文件字节正确只是本次读取的内容校验；只有全部通过后，才把真实文件
  // 读取量提交到对应统计字段。用户态复制或校验本身不计入文件字节。
  for(offset = 0; offset < AI_SHARD_BYTES; offset++)
    if((uchar)block[offset] != ai_byte(family, shard, offset))
      return -1;

  if(family == AI_MODEL_FAMILY)
    io->model_read_bytes += AI_SHARD_BYTES;
  else
    io->embed_read_bytes += AI_SHARD_BYTES;
  return 0;
}

static int
baseline_begin_kv_write(struct ai_session *session)
{
  // baseline 的 KV 写入阶段不需要长期保持一个 fd；每个请求单独创建文件。
  (void)session;
  return 0;
}

static int
baseline_store_kv(struct ai_session *session, int request,
                  struct kv_entry *kv, struct ai_io *io)
{
  char name[AI_KV_NAME_LEN];
  int fd;

  // worker 和 request 都编码进文件名，保证多个 worker 的同号请求不会
  // 互相覆盖。删除旧文件后重新创建，也能让重复运行从干净状态开始。
  ai_baseline_kv_name(name, session->worker, request);
  unlink(name);
  fd = open(name, O_CREATE | O_WRONLY);
  if(fd < 0)
    return -1;
  if(write_exact(fd, kv, AI_KV_FILE_BYTES) < 0) {
    // 部分 KV 不能被恢复为合法记录，因此失败路径必须关闭 fd 并删除文件。
    close(fd);
    unlink(name);
    return -1;
  }
  close(fd);
  // write_exact 成功后才提交固定大小的 I/O 统计，统计值不会被短写污染。
  io->kv_write_bytes += AI_KV_FILE_BYTES;
  return 0;
}

static int
baseline_end_kv_write(struct ai_session *session)
{
  (void)session;
  return 0;
}

static int
baseline_begin_kv_read(struct ai_session *session)
{
  // 读取阶段同样不保留共享文件偏移；restore_kv 每次按 request 找到文件。
  (void)session;
  return 0;
}

static int
baseline_prefetch_next(struct ai_session *session, int request)
{
  (void)session;
  (void)request;
  return 0;
}

static int
baseline_prefetch_wait(struct ai_session *session, int request)
{
  (void)session;
  (void)request;
  return 0;
}

static int
baseline_restore_kv(struct ai_session *session, int request,
                    struct kv_entry *kv, struct ai_io *io)
{
  char name[AI_KV_NAME_LEN];
  int fd;

  // workload 已经释放并重新申请了 KV 页面，这里从持久化文件恢复完整
  // 记录，而不是继续使用 prefill 阶段的旧指针或重新计算 K/V。
  ai_baseline_kv_name(name, session->worker, request);
  fd = open(name, O_RDONLY);
  if(fd < 0)
    return -1;
  if(read_exact(fd, kv, AI_KV_FILE_BYTES) < 0) {
    close(fd);
    return -1;
  }
  close(fd);
  // 只有完整读取成功才增加 kv_read_bytes；调用者随后还会检查 KV checksum。
  io->kv_read_bytes += AI_KV_FILE_BYTES;
  return 0;
}

static void
baseline_end(struct ai_session *session)
{
  char name[AI_KV_NAME_LEN];
  int request;

  // baseline 为每个请求创建了文件，所以结束时按同一 worker 的请求范围
  // 删除临时持久化状态，避免下一轮实验误读旧文件。
  for(request = 0; request < session->requests; request++) {
    ai_baseline_kv_name(name, session->worker, request);
    unlink(name);
  }
  memset(session, 0, sizeof(*session));
}

struct ai_path ai_baseline_path = {
  .mode = AI_MODE_BASELINE,
  .name = "baseline",
  .implemented = 1,
  .prefetch = 0,
  .begin = baseline_begin,
  .load = baseline_load,
  .begin_kv_write = baseline_begin_kv_write,
  .store_kv = baseline_store_kv,
  .end_kv_write = baseline_end_kv_write,
  .begin_kv_read = baseline_begin_kv_read,
  .prefetch_next = baseline_prefetch_next,
  .prefetch_wait = baseline_prefetch_wait,
  .restore_kv = baseline_restore_kv,
  .end = baseline_end,
};
