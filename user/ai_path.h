#ifndef XV6_AI_PATH_H
#define XV6_AI_PATH_H

#include "ai_assets.h"

enum ai_mode {
  AI_MODE_BASELINE,
  AI_MODE_STUDENT,
  AI_MODE_PREFETCH,
};

struct kv_entry {
  int key[AI_DIM];
  int value[AI_DIM];
};

#define AI_KV_FILE_BYTES (AI_KV_TOKENS * sizeof(struct kv_entry))

struct ai_io {
  uint model_read_bytes;
  uint embed_read_bytes;
  uint kv_write_bytes;
  uint kv_read_bytes;
};

// session 只属于一个 worker。state 和 memory 为学生实现预留，不规定优化方法。
struct ai_session {
  int worker;
  int requests;
  int state[8];
  void *memory;
};

struct ai_path {
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
