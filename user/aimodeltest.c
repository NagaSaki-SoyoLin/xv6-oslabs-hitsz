#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#include "ai_student_impl.h"

static char block[AI_SHARD_BYTES];

static void
fail(char *message)
{
  printf("AIMODELTEST: 失败：%s\n", message);
  exit(1);
}

static int
read_exact(int fd, void *buffer, int length)
{
  char *p = buffer;
  int n;

  while(length > 0) {
    n = read(fd, p, length);
    if(n <= 0)
      return -1;
    p += n;
    length -= n;
  }
  return 0;
}

static void
verify_manifest(void)
{
  struct ai_manifest manifest;
  int fd;

  fd = open("aimeta", O_RDONLY);
  if(fd < 0)
    fail("请先运行 modelprep");
  if(read_exact(fd, &manifest, sizeof(manifest)) < 0) {
    close(fd);
    fail("模型清单不完整");
  }
  close(fd);
  if(manifest.magic != AI_MAGIC || manifest.version != AI_VERSION ||
     manifest.model_shards != AI_MODEL_SHARDS ||
     manifest.embed_shards != AI_EMBED_SHARDS ||
     manifest.shard_bytes != AI_SHARD_BYTES ||
     manifest.checksum != ai_expected_checksum())
    fail("模型清单内容错误");
}

int
main(int argc, char *argv[])
{
  struct ai_session session;
  struct ai_io io;
  uint checksum = 0;
  uint family;
  uint shard;
  uint shards;
  int offset;

  (void)argc;
  (void)argv;
  verify_manifest();
  memset(&session, 0, sizeof(session));
  memset(&io, 0, sizeof(io));
  session.worker = 0;
  session.requests = 1;
  if(ai_student_model_begin(&session) < 0)
    fail("AI 附加题一：模型加载尚未实现");

  for(family = AI_MODEL_FAMILY; family <= AI_EMBED_FAMILY; family++) {
    shards = family == AI_MODEL_FAMILY ? AI_MODEL_SHARDS : AI_EMBED_SHARDS;
    for(shard = 0; shard < shards; shard++) {
      if(ai_student_model_load(&session, family, shard, block, &io) < 0) {
        ai_student_model_end(&session);
        fail("分片加载失败");
      }
      for(offset = 0; offset < AI_SHARD_BYTES; offset++) {
        if((uchar)block[offset] != ai_byte(family, shard, offset)) {
          ai_student_model_end(&session);
          fail("分片内容损坏");
        }
        checksum = ai_checksum_step(checksum, (uchar)block[offset]);
      }
    }
  }
  ai_student_model_end(&session);

  if(checksum != ai_expected_checksum())
    fail("汇总校验和错误");
  if(io.model_read_bytes != AI_MODEL_SHARDS * AI_SHARD_BYTES ||
     io.embed_read_bytes != AI_EMBED_SHARDS * AI_SHARD_BYTES)
    fail("实际文件读取字节数不正确");

  printf("AIMODELTEST: verify OK shards=%d checksum=%l\n",
         AI_TOTAL_SHARDS, (uint64)checksum);
  exit(0);
}
