#ifndef XV6_AI_PATH_H
#define XV6_AI_PATH_H

#include "ai_assets.h"

enum ai_mode {
  AI_MODE_BASELINE,
  AI_MODE_STUDENT,
  AI_MODE_PREFETCH,
};

struct kv_entry {
  // 一条 KV 记录由 AI_DIM 个 key/value 整数组成；AI_KV_TOKENS 条记录
  // 连续组成一次请求的 KV cache，并最终写入 AI_KV_FILE_BYTES 字节。
  int key[AI_DIM];
  int value[AI_DIM];
};

#define AI_KV_FILE_BYTES (AI_KV_TOKENS * sizeof(struct kv_entry))

struct ai_io {
  // 只统计成功完成的真实文件 I/O。缓存命中时复制数据不应重复增加
  // model/embed_read_bytes；KV 也必须完整写入或恢复后再提交固定字节数。
  uint model_read_bytes;
  uint embed_read_bytes;
  uint kv_write_bytes;
  uint kv_read_bytes;
};

// session 只属于一个 worker，不能在多个 worker 之间共享可写状态。state
// 和 memory 为学生实现预留，不规定缓存或 KV 文件的具体优化方法。
struct ai_session {
  int worker;
  int requests;
  int state[8];
  void *memory;
};

struct ai_path {
  // ai_path 是 workload 使用的统一操作表。baseline、student 和 prefetch
  // 共享同一调用顺序，只有函数内部的数据组织和并发方式可以不同。
  enum ai_mode mode;
  char *name;
  int implemented;
  int prefetch;
  int (*begin)(struct ai_session *, int, int);
  int (*load)(struct ai_session *, uint, uint, char *, struct ai_io *);
  int (*begin_kv_write)(struct ai_session *);
  int (*store_kv)(struct ai_session *, int, struct kv_entry *, struct ai_io *);
  int (*end_kv_write)(struct ai_session *);
  int (*begin_kv_read)(struct ai_session *);
  int (*prefetch_next)(struct ai_session *, int);
  int (*prefetch_wait)(struct ai_session *, int);
  int (*restore_kv)(struct ai_session *, int, struct kv_entry *, struct ai_io *);
  void (*end)(struct ai_session *);
};

extern struct ai_path ai_baseline_path;
extern struct ai_path ai_student_path;
extern struct ai_path ai_prefetch_path;

#endif
