#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#include "ai_assets.h"

// modelprep 是用户态的模型资产生产器：它把确定性字节真正写入 xv6
// 文件系统，供后续 aiinfer 通过 open/read 读取。它不是训练程序，也不
// 应被推理通路用来绕过文件系统直接生成模型内容。

static void
fail(char *message)
{
  printf("MODELPREP: 失败：%s\n", message);
  exit(1);
}

static int
write_exact(int fd, void *buffer, int length)
{
  char *p = buffer;
  int n;

  // write() 可能只完成部分请求；只有循环写完 length 字节，文件记录
  // 才算完整提交。短写或错误都交给调用者处理。
  while(length > 0) {
    n = write(fd, p, length);
    if(n <= 0)
      return -1;
    p += n;
    length -= n;
  }
  return 0;
}

static void
write_shard(char *name, uint family, uint shard, uint *checksum)
{
  char block[AI_SHARD_BYTES];
  int fd;
  int offset;

  // 先在用户页中生成一个完整分片，并按文件中的字节顺序累计总校验和。
  // 之后再一次性写入文件，aiinfer 会用同一个确定性函数逐字节核验。
  for(offset = 0; offset < AI_SHARD_BYTES; offset++) {
    block[offset] = ai_byte(family, shard, offset);
    *checksum = ai_checksum_step(*checksum, (uchar)block[offset]);
  }

  // 删除同名旧文件，保证重复运行 modelprep 不会沿用上次的残缺内容。
  unlink(name);
  fd = open(name, O_CREATE | O_WRONLY);
  if(fd < 0)
    fail("无法创建模型分片");
  if(write_exact(fd, block, sizeof(block)) < 0) {
    // 不完整分片不能留在文件系统中，否则后续错误会被推迟到推理读取阶段。
    close(fd);
    unlink(name);
    fail("模型分片写入不完整");
  }
  close(fd);
}

static void
remove_old_kv(void)
{
  char name[AI_KV_NAME_LEN];
  char stream[AI_STREAM_NAME_LEN];
  int worker;
  int request;

  // 每次生成模型前清理上次 workload 留下的 KV 文件，避免测试结果受到
  // 旧 worker 或旧请求文件的影响。模型分片本身由 write_shard 单独覆盖。
  for(worker = 0; worker < AI_MAX_WORKERS; worker++) {
    for(request = 0; request < AI_MAX_REQUESTS; request++) {
      ai_baseline_kv_name(name, worker, request);
      unlink(name);
    }
    ai_student_kv_name(stream, worker);
    unlink(stream);
  }
}

int
main(int argc, char *argv[])
{
  struct ai_manifest manifest;
  char name[AI_SHARD_NAME_LEN];
  uint checksum = 0;
  uint shard;
  int fd;

  (void)argc;
  (void)argv;
  // 先清理 KV，再按“模型分片 -> embedding 分片 -> aimeta 清单”的顺序
  // 生成资产；清单最后写入，表示前面的分片已经全部成功。
  remove_old_kv();

  for(shard = 0; shard < AI_MODEL_SHARDS; shard++) {
    ai_shard_name(name, AI_MODEL_FAMILY, shard);
    write_shard(name, AI_MODEL_FAMILY, shard, &checksum);
  }
  for(shard = 0; shard < AI_EMBED_SHARDS; shard++) {
    ai_shard_name(name, AI_EMBED_FAMILY, shard);
    write_shard(name, AI_EMBED_FAMILY, shard, &checksum);
  }
  // 清单中的 checksum 是所有分片内容的整体承诺。这里先在用户态检查，
  // 防止把生成错误的数据发布给后续实验。
  if(checksum != ai_expected_checksum())
    fail("生成数据的校验和错误");

  manifest.magic = AI_MAGIC;
  manifest.version = AI_VERSION;
  manifest.model_shards = AI_MODEL_SHARDS;
  manifest.embed_shards = AI_EMBED_SHARDS;
  manifest.shard_bytes = AI_SHARD_BYTES;
  manifest.checksum = checksum;

  // aimeta 描述数据格式和整体校验和，必须在所有分片成功后才创建。
  unlink("aimeta");
  fd = open("aimeta", O_CREATE | O_WRONLY);
  if(fd < 0)
    fail("无法创建模型清单");
  if(write_exact(fd, &manifest, sizeof(manifest)) < 0) {
    // 清单未完整写入时删除它，避免 aiinfer 把半条清单当成合法资产。
    close(fd);
    unlink("aimeta");
    fail("模型清单写入不完整");
  }
  close(fd);

  printf("MODELPREP: OK shards=%d bytes=%d checksum=%l\n",
         AI_TOTAL_SHARDS, AI_TOTAL_SHARDS * AI_SHARD_BYTES,
         (uint64)checksum);
  exit(0);
}
