#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#include "ai_assets.h"

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

  for(offset = 0; offset < AI_SHARD_BYTES; offset++) {
    block[offset] = ai_byte(family, shard, offset);
    *checksum = ai_checksum_step(*checksum, (uchar)block[offset]);
  }

  unlink(name);
  fd = open(name, O_CREATE | O_WRONLY);
  if(fd < 0)
    fail("无法创建模型分片");
  if(write_exact(fd, block, sizeof(block)) < 0) {
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
  remove_old_kv();

  for(shard = 0; shard < AI_MODEL_SHARDS; shard++) {
    ai_shard_name(name, AI_MODEL_FAMILY, shard);
    write_shard(name, AI_MODEL_FAMILY, shard, &checksum);
  }
  for(shard = 0; shard < AI_EMBED_SHARDS; shard++) {
    ai_shard_name(name, AI_EMBED_FAMILY, shard);
    write_shard(name, AI_EMBED_FAMILY, shard, &checksum);
  }
  if(checksum != ai_expected_checksum())
    fail("生成数据的校验和错误");

  manifest.magic = AI_MAGIC;
  manifest.version = AI_VERSION;
  manifest.model_shards = AI_MODEL_SHARDS;
  manifest.embed_shards = AI_EMBED_SHARDS;
  manifest.shard_bytes = AI_SHARD_BYTES;
  manifest.checksum = checksum;

  unlink("aimeta");
  fd = open("aimeta", O_CREATE | O_WRONLY);
  if(fd < 0)
    fail("无法创建模型清单");
  if(write_exact(fd, &manifest, sizeof(manifest)) < 0) {
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
