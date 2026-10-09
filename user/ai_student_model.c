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

int
ai_student_model_begin(struct ai_session *session)
{
  // TODO(LAB3-AI，附加题一)：初始化当前 worker 的模型加载状态。
  // 可以在这里申请 worker 私有缓存；不得修改全局模型文件或扩大 NBUF。
  // 单项验证：先运行 modelprep，再运行 aimodeltest。
  int shards = AI_MODEL_SHARDS + AI_EMBED_SHARDS; // 总分片数
  uchar **cache; // 缓存块指针
  int i, j;

  cache = malloc(shards * sizeof(uchar *)); // 分配缓存块
  if(cache == 0)
    return -1;
  memset(cache, 0, shards * sizeof(uchar *)); // 清零
  for(i = 0; i < shards; i++) {
    cache[i] = malloc(AI_SHARD_BYTES + 4); // 后四个字节标记是否有缓存
    if(cache[i] == 0) {
      for(j = 0; j < shards; j++) // 没申请到的格子还是 0
        if(cache[j])
          free(cache[j]);
      free(cache);
      return -1;
    }
    memset(cache[i], 0, AI_SHARD_BYTES + 4);
  }
  session->memory = cache; // 全部成功才登记
  return 0;
}

int
ai_student_model_load(struct ai_session *session, uint family, uint shard,
                      char *block, struct ai_io *io)
{
  // TODO(LAB3-AI，附加题一)：加载一个模型块或 embedding shard。
  // 首次数据必须来自 xv6 文件系统，成功时 block 中必须恰好有
  // AI_SHARD_BYTES 字节；只有真实读取成功后才能更新 io 字节数。
  // 建议先读懂 ai_baseline.c 的经典 open-read-close 数据流，再考虑缓存。
  char name[AI_SHARD_NAME_LEN];
  int fd;
  int offset;
  uchar **cache; // 缓存块指针
  int id; // 缓存块下标
  uint *isloaded; // 缓存块标记

  if ((family == AI_MODEL_FAMILY && shard >= AI_MODEL_SHARDS) ||
     (family == AI_EMBED_FAMILY && shard >= AI_EMBED_SHARDS))
    return -1;

  ai_shard_name(name, family, shard);

  cache = session->memory;
  id = (name[0] == 'm' ? 0 : AI_MODEL_SHARDS) + ((name[1] - '0') * 10 + (name[2] - '0'));
  isloaded = (uint *)(cache[id] + AI_SHARD_BYTES);
  if (*isloaded == 1) { // 如果缓存命中
    memcpy(block, cache[id], AI_SHARD_BYTES);
    return 0;
  }

  fd = open(name, O_RDONLY);
  if(fd < 0)
    return -1;
  if(read_exact(fd, cache[id], AI_SHARD_BYTES) < 0) {
    close(fd);
    return -1;
  }
  close(fd);

  for(offset = 0; offset < AI_SHARD_BYTES; offset++)
    if(cache[id][offset] != ai_byte(family, shard, offset))
      return -1;

  if(family == AI_MODEL_FAMILY)
    io->model_read_bytes += AI_SHARD_BYTES;
  else
    io->embed_read_bytes += AI_SHARD_BYTES;

  *isloaded = 1; // 存入缓存
  memcpy(block, cache[id], AI_SHARD_BYTES);
  return 0;
}

void
ai_student_model_end(struct ai_session *session)
{
  // TODO(LAB3-AI，附加题一)：释放缓存并关闭尚未关闭的模型文件描述符。
  // 失败路径和正常路径都必须能够安全调用本函数，资源只能释放一次。
  int shards = AI_MODEL_SHARDS + AI_EMBED_SHARDS;
  uchar **cache = session->memory;
  int i;

  if(cache == 0) { // begin 没成功过或者已经清理过
    memset(session, 0, sizeof(*session));
    return;
  }
  for(i = 0; i < shards; i++)
    if(cache[i])
      free(cache[i]);
  free(cache);
  memset(session, 0, sizeof(*session));
}
