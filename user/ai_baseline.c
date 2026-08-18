#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#include "ai_path.h"

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

static int
baseline_begin(struct ai_session *session, int worker, int requests)
{
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
  (void)session;
  return 0;
}

static int
baseline_store_kv(struct ai_session *session, int request,
                  struct kv_entry *kv, struct ai_io *io)
{
  char name[AI_KV_NAME_LEN];
  int fd;

  ai_baseline_kv_name(name, session->worker, request);
  unlink(name);
  fd = open(name, O_CREATE | O_WRONLY);
  if(fd < 0)
    return -1;
  if(write_exact(fd, kv, AI_KV_FILE_BYTES) < 0) {
    close(fd);
    unlink(name);
    return -1;
  }
  close(fd);
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

  ai_baseline_kv_name(name, session->worker, request);
  fd = open(name, O_RDONLY);
  if(fd < 0)
    return -1;
  if(read_exact(fd, kv, AI_KV_FILE_BYTES) < 0) {
    close(fd);
    return -1;
  }
  close(fd);
  io->kv_read_bytes += AI_KV_FILE_BYTES;
  return 0;
}

static void
baseline_end(struct ai_session *session)
{
  char name[AI_KV_NAME_LEN];
  int request;

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
