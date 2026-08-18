#ifndef XV6_AI_ASSETS_H
#define XV6_AI_ASSETS_H

#define AI_MAGIC 0x41494c33U
#define AI_VERSION 3U

#define AI_MODEL_FAMILY 1U
#define AI_EMBED_FAMILY 2U
#define AI_MODEL_SHARDS 37
#define AI_EMBED_SHARDS 37
#define AI_TOTAL_SHARDS (AI_MODEL_SHARDS + AI_EMBED_SHARDS)
#define AI_SHARD_BYTES 1024

#define AI_VOCAB 32
#define AI_DIM 8
#define AI_KV_TOKENS 32
#define AI_MAX_WORKERS 3
#define AI_MAX_REQUESTS 24

#define AI_SHARD_NAME_LEN 4
#define AI_KV_NAME_LEN 7
#define AI_STREAM_NAME_LEN 5

struct ai_manifest {
  uint magic;
  uint version;
  uint model_shards;
  uint embed_shards;
  uint shard_bytes;
  uint checksum;
};

// 文件内容由确定性函数生成，便于在不同机器上验证相同输入。
// 被测推理链路仍必须从 xv6 文件系统读取这些字节。
static inline uchar
ai_byte(uint family, uint shard, uint offset)
{
  return (family * 97 + shard * 31 + offset * 17 + 11) & 0xff;
}

static inline uint
ai_checksum_step(uint checksum, uchar value)
{
  return checksum * 33 + value;
}

static inline uint
ai_expected_checksum(void)
{
  uint checksum = 0;
  uint family;
  uint shard;
  uint shards;
  uint offset;

  for(family = AI_MODEL_FAMILY; family <= AI_EMBED_FAMILY; family++) {
    shards = family == AI_MODEL_FAMILY ? AI_MODEL_SHARDS : AI_EMBED_SHARDS;
    for(shard = 0; shard < shards; shard++)
      for(offset = 0; offset < AI_SHARD_BYTES; offset++)
        checksum = ai_checksum_step(checksum,
                                    ai_byte(family, shard, offset));
  }
  return checksum;
}

static inline void
ai_shard_name(char *name, uint family, uint shard)
{
  name[0] = family == AI_MODEL_FAMILY ? 'm' : 'e';
  name[1] = '0' + shard / 10;
  name[2] = '0' + shard % 10;
  name[3] = 0;
}

// baseline 为每个请求建立独立文件，例如 kvb007 表示 worker 0 的请求 7。
static inline void
ai_baseline_kv_name(char *name, int worker, int request)
{
  name[0] = 'k';
  name[1] = 'v';
  name[2] = 'b';
  name[3] = '0' + worker;
  name[4] = '0' + request / 10;
  name[5] = '0' + request % 10;
  name[6] = 0;
}

// student/选做通路可按 worker 使用顺序流文件，例如 kvs0。
static inline void
ai_student_kv_name(char *name, int worker)
{
  name[0] = 'k';
  name[1] = 'v';
  name[2] = 's';
  name[3] = '0' + worker;
  name[4] = 0;
}

#endif
